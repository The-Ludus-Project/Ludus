#pragma once

/// @file
/// @brief Collision world: posed colliders that lease shapes, filters, protected read views and world queries.
///
/// See "Geometry store and collision world" in docs/architecture/physics-collision.md. World-space values carry
/// the suffix `World`, in meters.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

namespace ludus::physics::collision
{
/// Handle domain marker for colliders in a CollisionWorld; never instantiated.
struct ColliderTag final
{
};

/// @brief Reference to one collider incarnation in one CollisionWorld.
///
/// Destroying a collider advances its slot's generation, so every copy of the handle becomes stale.
using ColliderHandle = foundation::identity::Handle<ColliderTag>;

/// Largest number of collider slots a world admits.
inline constexpr uint32 MAX_COLLISION_WORLD_COLLIDERS = 1U << 20U;

/// Largest number of colliders one query may ignore explicitly.
inline constexpr uint32 MAX_QUERY_IGNORED_COLLIDERS = 64;

/// Every collision category bit.
inline constexpr uint32 ALL_COLLISION_CATEGORIES = 0xFFFFFFFFU;

/// @brief Rigid placement in world space: rotate by OrientationWorld, then translate to PositionWorld.
///
/// Admitted values: finite position components within MAX_COORDINATE_METERS; a finite orientation whose squared
/// norm is within UNIT_QUATERNION_SQUARED_TOLERANCE of one (renormalized in float64).
struct RigidPose final
{
    foundation::math::Vector3d PositionWorld{};      ///< Origin of the shape's local space, meters.
    foundation::math::Quaternion OrientationWorld{}; ///< Rotation from local axes to world axes.
};

/// @brief How a collider takes part in simulation and queries.
///
/// IsSolid and IsSensor are mutually exclusive. Query participation is independent of both. A collider that is
/// neither solid nor a sensor is query-only.
struct ColliderParticipation final
{
    bool IsSolid = true;     ///< Generates solid contacts once rigid bodies exist.
    bool IsSensor = false;   ///< Reports occupancy without response once sensors exist.
    bool IsQueryable = true; ///< Visible to world queries.
};

/// @brief Category bits and the categories a collider collides with, for symmetric simulation pair filtering.
struct CollisionFilter final
{
    uint32 CategoryBits = 1;                         ///< Categories this collider belongs to; at least one bit.
    uint32 CollisionMask = ALL_COLLISION_CATEGORIES; ///< Categories this collider accepts as simulation partners.
};

/// @brief Symmetric simulation pair filter: each collider must accept one of the other's categories.
[[nodiscard]] constexpr bool ShouldCollide(const CollisionFilter& first, const CollisionFilter& second) noexcept
{
    return (first.CategoryBits & second.CollisionMask) != 0 && (second.CategoryBits & first.CollisionMask) != 0;
}

/// @brief Description of a new collider.
struct ColliderDescription final
{
    ShapeHandle Shape{};                   ///< Live shape of the world's store; the collider takes its own lease.
    RigidPose Pose{};                      ///< World placement of the shape's local space.
    ColliderParticipation Participation{}; ///< Simulation and query participation.
    CollisionFilter Filter{};              ///< Simulation categories and mask.
    bool IsEnabled = true;                 ///< Disabled colliders take part in nothing.
    uint64 Binding = 0;                    ///< Opaque game value copied into query results, for example an entity.
};

/// @brief Directed query filter: which colliders a query may report.
///
/// A collider is reported only when it is live, enabled and queryable; at least one of its category bits is in
/// CategoryMask; it is not a sensor unless ShouldIncludeSensors; and it is not listed in IgnoredColliders. The
/// ignored view is borrowed for the query call and holds at most MAX_QUERY_IGNORED_COLLIDERS handles.
struct QueryFilter final
{
    uint32 CategoryMask = ALL_COLLISION_CATEGORIES; ///< Target categories the query accepts.
    bool ShouldIncludeSensors = false;              ///< Whether sensor colliders may be reported.
    // Explicit so designated initializers may omit it without -Wmissing-designated-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const ColliderHandle> IgnoredColliders{}; ///< Colliders never reported.
};

/// @brief Closest world raycast request. Admitted values follow RaycastInput, in world space.
struct WorldRaycastInput final
{
    foundation::math::Vector3d OriginWorld{};                     ///< Ray origin, meters.
    foundation::math::Vector3d DirectionWorld{0.0, 0.0, -1.0};    ///< Unit ray direction.
    float64 MaxDistanceMeters = 0.0;                              ///< Inclusive distance limit along the ray.
    TriangleFacePolicy FacePolicy = TriangleFacePolicy::TwoSided; ///< Faces the ray may hit.
    QueryFilter Filter{};                                         ///< Colliders the query may report.
    QueryTraversal Traversal = QueryTraversal::Hierarchy; ///< Hierarchy, or brute force over colliders and triangles.
};

/// @brief Closest world raycast answer.
///
/// On a miss `HasHit` is false and every other field keeps its default value. Equal distances resolve to the lower
/// collider slot, then generation, then the lower source triangle index.
///
/// Mesh colliders follow RaycastResult. A convex collider is a closed solid: its hit is the entry point, with the
/// outward surface normal and IsFrontFace set, decided with the convex cast's contact tolerance. A ray that starts
/// inside a convex collider hits it at distance zero with no normal and IsFrontFace false. FacePolicy BackFacesOnly
/// never reports convex colliders; request an exit query instead.
struct WorldRaycastResult final
{
    bool HasHit = false;                        ///< Whether any reported collider lies within the limit.
    float64 DistanceMeters = 0.0;               ///< Ray parameter of the hit, meters.
    foundation::math::Vector3d PositionWorld{}; ///< `OriginWorld + DirectionWorld * DistanceMeters`.
    bool HasNormal = false;                     ///< Whether NormalWorld is valid; false for a start inside a solid.
    /// Unit normal in world axes: a mesh triangle's winding normal, or a convex collider's outward surface normal.
    foundation::math::Vector3d NormalWorld{};
    bool IsFrontFace = false; ///< Whether the ray direction points against NormalWorld.
    /// Source triangle index in a mesh collider; INVALID_TRIANGLE_INDEX for a convex collider.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
    ColliderHandle Collider{}; ///< The hit collider.
    uint64 Binding = 0;        ///< The hit collider's binding.
};

/// @brief Closest world convex cast request. Admitted values follow ConvexCastInput, in world space.
struct WorldConvexCastInput final
{
    ConvexShape Shape{}; ///< Shape to cast when QueryShape is null; a hull is borrowed for this call.
    /// When not null, a live sphere, box, capsule or hull of the world's store, cast instead of Shape. The read view
    /// protects the store, so the stored shape cannot change during the query.
    ShapeHandle QueryShape{};
    foundation::math::Vector3d PositionWorld{};      ///< Shape origin at the start of the cast, meters.
    foundation::math::Quaternion OrientationWorld{}; ///< Rotation from shape axes to world axes.
    foundation::math::Vector3d TranslationWorld{};   ///< Full cast displacement, meters.
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report; ///< Treatment of starting overlaps.
    QueryFilter Filter{};                                              ///< Colliders the query may report.
    QueryTraversal Traversal = QueryTraversal::Hierarchy; ///< Hierarchy, or brute force over colliders and triangles.
};

/// @brief Closest world convex cast answer.
///
/// Contact rules follow ConvexCastResult within each collider. Ordering is smaller fraction, then an initial overlap
/// before a touching contact, then the lower collider slot and generation, then the lower source triangle index.
struct WorldConvexCastResult final
{
    bool HasHit = false;                               ///< Whether the shape contacts or overlaps a reported collider.
    float64 Fraction = 0.0;                            ///< Fraction of the translation in [0, 1) at first contact.
    float64 DistanceMeters = 0.0;                      ///< `Fraction * Length(TranslationWorld)`.
    bool HasStartedOverlapping = false;                ///< The shape overlapped the collider at its starting pose.
    bool HasNormal = false;                            ///< Whether NormalWorld is valid.
    foundation::math::Vector3d NormalWorld{};          ///< Unit separating normal, from the collider toward the shape.
    bool HasWitness = false;                           ///< Whether WitnessPositionWorld is valid.
    foundation::math::Vector3d WitnessPositionWorld{}; ///< Closest collider point at first contact.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;     ///< Source triangle index in the collider's mesh.
    ColliderHandle Collider{};                         ///< The hit collider.
    uint64 Binding = 0;                                ///< The hit collider's binding.
};

/// @brief Fixed capacity of a CollisionWorld, chosen at initialization.
struct CollisionWorldSettings final
{
    uint32 MaxColliders = 0; ///< Collider slots to reserve, in [1, MAX_COLLISION_WORLD_COLLIDERS].
};

/// @brief How many overlapping colliders an overlap query looks for.
enum class OverlapMode : uint8
{
    Any, ///< Stop at the first overlapping collider found; promises no particular one.
    All, ///< Find every overlapping collider and record as many as the buffer holds.
};

/// @brief World overlap request: which reported colliders a stationary posed convex shape overlaps.
///
/// Admitted values follow ConvexCastInput with no translation. A collider overlaps when the shape is provably deeper
/// than the convex cast's contact tolerance inside it, exactly the rule for a cast's initial overlap; touching is
/// not overlapping. Broadphase bounds are only candidates.
struct WorldOverlapInput final
{
    ConvexShape Shape{}; ///< Shape to test when QueryShape is null; a hull is borrowed for this call.
    /// When not null, a live sphere, box, capsule or hull of the world's store, tested instead of Shape.
    ShapeHandle QueryShape{};
    foundation::math::Vector3d PositionWorld{};           ///< Shape origin, meters.
    foundation::math::Quaternion OrientationWorld{};      ///< Rotation from shape axes to world axes.
    OverlapMode Mode = OverlapMode::All;                  ///< Any or All.
    QueryFilter Filter{};                                 ///< Colliders the query may report.
    QueryTraversal Traversal = QueryTraversal::Hierarchy; ///< Hierarchy, or brute force over colliders and triangles.
};

/// @brief One overlapping collider.
struct ColliderOverlap final
{
    ColliderHandle Collider{}; ///< The overlapping collider.
    uint64 Binding = 0;        ///< Its binding.
    /// Lowest overlapping source triangle of a mesh collider; INVALID_TRIANGLE_INDEX for a convex collider.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
    uint32 TriangleCount = 0; ///< Overlapping triangles of a mesh collider; zero for a convex collider.
};

/// @brief Completion of an overlap query.
///
/// Written records are sorted by collider slot and generation. IsComplete is true exactly when every overlap the
/// mode looks for was written: an All query whose buffer was too small reports IsComplete false with OverlapCount,
/// the number it found, so the caller can retry with a larger buffer; the written records are then the first
/// overlaps the hierarchy search found, sorted, not a nearest subset. An Any query finds at most one overlap.
struct WorldOverlapResult final
{
    bool HasOverlap = false; ///< Whether any reported collider overlaps the shape.
    usize OverlapCount = 0;  ///< Overlapping colliders found: all of them for All, at most one for Any.
    usize WrittenCount = 0;  ///< Records written to the buffer, a prefix of the buffer.
    bool IsComplete = true;  ///< Whether WrittenCount equals OverlapCount.
};

/// @brief Completion of a ray or convex-cast query that records every hit collider.
///
/// An All query reports each reported collider it hits once, with that collider's closest hit. It writes records
/// until the buffer is full and keeps counting, then sorts the written records by distance or fraction (an initial
/// overlap first), collider slot, generation and triangle. When the buffer was too small the written records are the
/// first hits the search found, sorted: a subset, not the nearest ones. Retry with HitCount records for all of them.
struct WorldHitsResult final
{
    bool HasHit = false;    ///< Whether any reported collider was hit.
    usize HitCount = 0;     ///< Hit colliders found.
    usize WrittenCount = 0; ///< Records written to the buffer, a sorted prefix of the buffer.
    bool IsComplete = true; ///< Whether WrittenCount equals HitCount.
};

class CollisionWorld;

/// @brief Scoped, protected read access to a CollisionWorld.
///
/// While any view of a world is active, the world refuses every mutation with Status::InvalidPhase, and so does the
/// world's GeometryStore, together with collider creation and destruction in every other world that shares it. The
/// colliders and shapes a query reads, or names by handle, therefore cannot change or be destroyed. A view records the
/// world's epoch when it is acquired; the epoch is the same for every query through the view.
///
/// Thread affinity: acquire, move and release views on the world's owner thread. While a view is active its const
/// queries may run concurrently on any threads. A view is move-only; destroying or releasing it ends the protection.
class CollisionReadView final
{
public:
    /// Construct an inactive view. Queries through it fail with Status::InvalidPhase.
    CollisionReadView() noexcept = default;
    /// Views are move-only: each active view is one registered reader.
    CollisionReadView(const CollisionReadView&) = delete;
    /// Views are move-only: each active view is one registered reader.
    CollisionReadView& operator=(const CollisionReadView&) = delete;
    /// Transfer the protection; `other` becomes inactive.
    CollisionReadView(CollisionReadView&& other) noexcept;
    /// Release this view's protection, then take `other`'s; `other` becomes inactive.
    CollisionReadView& operator=(CollisionReadView&& other) noexcept;
    /// Release the protection.
    ~CollisionReadView() noexcept;

    /// End the protection early. Does nothing for an inactive view.
    void Release() noexcept;

    /// Whether the view currently protects a world.
    [[nodiscard]] LUDUS_INLINE bool IsActive() const noexcept
    {
        return mWorldOrNull != nullptr;
    }
    /// World epoch at acquisition; zero for an inactive view.
    [[nodiscard]] LUDUS_INLINE uint64 GetEpoch() const noexcept
    {
        return mEpoch;
    }

    /// @brief Find the nearest reported collider triangle along a ray.
    /// @param input Ray, limit, face policy, filter and traversal, in world space.
    /// @param outResult Receives the hit or an explicit miss on Success; unchanged on failure.
    /// @return Success; InvalidPhase for an inactive view; the RaycastInput validation failures; InvalidArgument for
    /// an invalid or oversized ignored-collider view.
    [[nodiscard]] Status RaycastClosest(const WorldRaycastInput& input, WorldRaycastResult& outResult) const noexcept;

    /// @brief Find any reported collider triangle or solid along a ray; stops at the first one found.
    ///
    /// The answer is a genuine hit within the limit but neither the nearest nor a stable choice: a mesh collider
    /// reports the first hit its traversal finds. A miss is certified only after every candidate was searched, which
    /// is always the case when the result is a miss.
    /// @param input As for RaycastClosest.
    /// @param outResult Receives a hit or an explicit miss on Success; unchanged on failure.
    /// @return As for RaycastClosest.
    [[nodiscard]] Status RaycastAny(const WorldRaycastInput& input, WorldRaycastResult& outResult) const noexcept;

    /// @brief Find every reported collider a ray hits, each at its closest hit.
    /// @param input As for RaycastClosest.
    /// @param hitBuffer Caller storage for the records, borrowed for this call; may be empty.
    /// @param outResult Receives the counts and completion on Success (see WorldHitsResult); unchanged on failure.
    /// @return As for RaycastClosest, plus InvalidArgument for an invalid buffer view. After a failure the buffer's
    /// contents are unspecified.
    [[nodiscard]] Status RaycastAll(const WorldRaycastInput& input,
                                    foundation::BorrowedView<WorldRaycastResult> hitBuffer,
                                    WorldHitsResult& outResult) const noexcept;

    /// @brief Find the first reported collider a posed convex shape contacts while translating.
    /// @param input Shape, pose, translation, overlap policy, filter and traversal, in world space.
    /// @param outResult Receives the hit or an explicit miss on Success; unchanged on failure.
    /// @return Success; InvalidPhase for an inactive view; InvalidHandle for a QueryShape that is not live in the
    /// world's store, or Unsupported for a triangle-mesh one; the ConvexCastInput validation failures;
    /// InvalidArgument for an invalid ignored-collider view; InitialOverlap under InitialOverlapPolicy::Reject;
    /// NotConverged; or
    /// OutOfRange when the shape, expressed in a reached collider's local space, lies outside the coordinate
    /// envelope where its path enters that collider's bounds.
    [[nodiscard]] Status CastConvexClosest(const WorldConvexCastInput& input,
                                           WorldConvexCastResult& outResult) const noexcept;

    /// @brief Find any reported collider a posed convex shape contacts while translating; stops at the first found.
    ///
    /// The answer follows the contact rules of CastConvexClosest but is neither the earliest contact nor a stable
    /// choice. Under InitialOverlapPolicy::Reject every candidate is still searched, because any overlap must fail the
    /// query.
    /// @param input As for CastConvexClosest.
    /// @param outResult Receives a hit or an explicit miss on Success; unchanged on failure.
    /// @return As for CastConvexClosest.
    [[nodiscard]] Status CastConvexAny(const WorldConvexCastInput& input,
                                       WorldConvexCastResult& outResult) const noexcept;

    /// @brief Find every reported collider a posed convex shape contacts while translating, each at its first contact.
    /// @param input As for CastConvexClosest.
    /// @param hitBuffer Caller storage for the records, borrowed for this call; may be empty.
    /// @param outResult Receives the counts and completion on Success (see WorldHitsResult); unchanged on failure.
    /// @return As for CastConvexClosest, plus InvalidArgument for an invalid buffer view. After a failure the
    /// buffer's contents are unspecified.
    [[nodiscard]] Status CastConvexAll(const WorldConvexCastInput& input,
                                       foundation::BorrowedView<WorldConvexCastResult> hitBuffer,
                                       WorldHitsResult& outResult) const noexcept;

    /// @brief Find the reported colliders a stationary posed convex shape overlaps.
    /// @param input Shape, pose, mode, filter and traversal, in world space.
    /// @param overlapBuffer Caller storage for the records, borrowed for this call; may be empty.
    /// @param outResult Receives the counts and completion on Success; unchanged on failure.
    /// @return Success; InvalidPhase for an inactive view; InvalidHandle or Unsupported for the QueryShape, as for
    /// CastConvexClosest; the ConvexCastInput validation failures; InvalidArgument for an unknown mode, an invalid
    /// buffer view or an invalid ignored-collider view; NotConverged; or OutOfRange when
    /// the shape reaches a collider's bounds from outside the coordinate envelope of its local space. After a failure
    /// the buffer's contents are unspecified.
    [[nodiscard]] Status Overlap(const WorldOverlapInput& input,
                                 foundation::BorrowedView<ColliderOverlap> overlapBuffer,
                                 WorldOverlapResult& outResult) const noexcept;

private:
    friend class CollisionWorld;
    friend class CollisionDebugExtraction;

    CollisionWorld* mWorldOrNull = nullptr;
    uint64 mEpoch = 0;
};

/// @brief A set of posed colliders over one GeometryStore, with filtered queries through protected read views.
///
/// The world reserves every collider slot when it is initialized, so creating, changing and destroying colliders
/// never allocate. Each collider takes its own lease on its shape and releases it when destroyed. Colliders may use
/// any stored shape: triangle meshes, hulls and sphere, box and capsule primitives. A dynamic bounding volume
/// hierarchy over the colliders' world bounds prunes queries; creating, moving and destroying a collider update it
/// in logarithmic time.
///
/// Thread affinity: initialization and mutation happen on one owner thread, and only while no read view is active.
/// The world is neither copyable nor movable, because read views keep its address.
class CollisionWorld final
{
public:
    /// Construct an uninitialized world. Does not allocate.
    CollisionWorld() noexcept = default;
    /// Not copyable: colliders have one owner.
    CollisionWorld(const CollisionWorld&) = delete;
    /// Not copyable: colliders have one owner.
    CollisionWorld& operator=(const CollisionWorld&) = delete;
    /// Not movable: read views keep the world's address.
    CollisionWorld(CollisionWorld&&) = delete;
    /// Not movable: read views keep the world's address.
    CollisionWorld& operator=(CollisionWorld&&) = delete;
    /// Destroy every collider, releasing its shape lease.
    /// @pre No read view of this world is active, and the store is still alive; checked by assertion.
    ~CollisionWorld() noexcept;

    /// @brief Reserve every collider slot, obtain the world's owner token and attach to `store`.
    /// @param inoutStore Initialized store that must outlive this world; it records the attachment.
    /// @param settings Capacity in [1, MAX_COLLISION_WORLD_COLLIDERS].
    /// @return Success; InvalidPhase when already initialized or the store is uninitialized; InvalidArgument for zero
    /// capacity; CapacityExceeded above the limit; AllocationFailure; or IdentityExhausted. Unchanged on failure.
    [[nodiscard]] Status TryInitialize(GeometryStore& inoutStore, const CollisionWorldSettings& settings) noexcept;

    /// @brief Create a collider.
    /// @param description Shape, pose, participation, filter, enabled state and binding.
    /// @param outCollider Receives the new handle on success; unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized or a read view of this world, or of any world sharing the
    /// store, is active; InvalidHandle for a shape that is
    /// not live in the store; NonFiniteInput, OutOfRange or
    /// InvalidArgument for the pose; InvalidArgument for a solid sensor or zero category bits; or CapacityExceeded
    /// when no slot is free or the shape's lease count is full.
    [[nodiscard]] Status TryCreateCollider(const ColliderDescription& description,
                                           ColliderHandle& outCollider) noexcept;

    /// @brief Destroy a collider and release its shape lease. Every copy of the handle becomes stale.
    /// @return Success; InvalidPhase when uninitialized or a read view of this world, or of any world sharing the
    /// store, is active; or InvalidHandle.
    [[nodiscard]] Status TryDestroyCollider(ColliderHandle collider) noexcept;

    /// @brief Move a collider.
    /// @return Success; InvalidPhase; InvalidHandle; or the pose validation failure. Unchanged on failure.
    [[nodiscard]] Status TrySetColliderPose(ColliderHandle collider, const RigidPose& pose) noexcept;

    /// @brief Enable or disable a collider.
    /// @return Success; InvalidPhase; or InvalidHandle.
    [[nodiscard]] Status TrySetColliderEnabled(ColliderHandle collider, bool isEnabled) noexcept;

    /// @brief Read back a live collider's description.
    /// @param collider Handle to resolve.
    /// @param outDescription Receives the shape, pose, participation, filter, enabled state and binding; unchanged on
    /// failure.
    /// @return Success, or InvalidHandle.
    [[nodiscard]] Status TryGetCollider(ColliderHandle collider, ColliderDescription& outDescription) const noexcept;

    /// @brief Begin a read phase.
    /// @param outView Receives the protection on success; must be inactive. Unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized; or InvalidArgument when outView is already active.
    [[nodiscard]] Status TryAcquireReadView(CollisionReadView& outView) noexcept;

    /// Whether `collider` names a live collider of this world.
    [[nodiscard]] bool IsAlive(ColliderHandle collider) const noexcept;

    /// Whether the world has been initialized.
    [[nodiscard]] LUDUS_INLINE bool IsInitialized() const noexcept
    {
        return mOwner != 0;
    }
    /// Number of live colliders.
    [[nodiscard]] LUDUS_INLINE usize GetColliderCount() const noexcept
    {
        return mLiveCount;
    }
    /// Counter advanced by every successful mutation; read views record it.
    [[nodiscard]] LUDUS_INLINE uint64 GetEpoch() const noexcept
    {
        return mEpoch;
    }
    /// Number of active read views.
    [[nodiscard]] LUDUS_INLINE uint32 GetReaderCount() const noexcept
    {
        return mReaderCount;
    }
    /// Height of the collider hierarchy: zero when empty, one for a single collider. A diagnostic of tree quality.
    [[nodiscard]] uint32 GetColliderHierarchyHeight() const noexcept;

private:
    /// One collider slot.
    struct Slot final
    {
        ColliderDescription Description{};           ///< The collider as created, with its current pose and state.
        const TriangleMesh* MeshOrNull = nullptr;    ///< The leased mesh; null for a convex collider or a free slot.
        ConvexShape Convex{};                        ///< The leased convex shape when MeshOrNull is null.
        foundation::math::Vector3d BoundsMinLocal{}; ///< Exact bounds of the shape in its local frame.
        foundation::math::Vector3d BoundsMaxLocal{}; ///< Upper corner of BoundsMinLocal's box.
        foundation::math::Vector3d AxisXWorld{1.0, 0.0, 0.0};       ///< Local +X in world axes.
        foundation::math::Vector3d AxisYWorld{0.0, 1.0, 0.0};       ///< Local +Y in world axes.
        foundation::math::Vector3d AxisZWorld{0.0, 0.0, 1.0};       ///< Local +Z in world axes.
        foundation::math::Vector3d BoundsMinWorld{};                ///< Conservative world bounds of the mesh.
        foundation::math::Vector3d BoundsMaxWorld{};                ///< Upper corner of BoundsMinWorld's box.
        uint32 Generation = foundation::identity::FIRST_GENERATION; ///< Current incarnation of the slot.
        uint32 Leaf = 0;                                            ///< The slot's hierarchy leaf while live.
        bool IsLive = false;                                        ///< Whether the slot holds a collider.
    };

    /// One node of the collider hierarchy: a leaf holds one collider slot, an internal node two children.
    struct HierarchyNode final
    {
        foundation::math::Vector3d BoundsMinWorld{}; ///< Lower corner of the bounds of every collider below.
        foundation::math::Vector3d BoundsMaxWorld{}; ///< Upper corner of BoundsMinWorld's box.
        uint32 Parent = 0;                           ///< Parent node, or the null node for the root.
        uint32 Left = 0;                             ///< First child of an internal node.
        uint32 Right = 0;                            ///< Second child of an internal node.
        uint32 Slot = 0;                             ///< Collider slot of a leaf.
        uint32 Height = 0;                           ///< Zero for a leaf; one more than the taller child otherwise.
    };

    friend class CollisionReadView;
    friend class CollisionWorldQueries;
    friend class ColliderHierarchy;
    friend class CollisionDebugExtraction;

    [[nodiscard]] Slot* findLive(ColliderHandle collider) noexcept;
    [[nodiscard]] const Slot* findLive(ColliderHandle collider) const noexcept;
    [[nodiscard]] Status checkMutable() const noexcept;
    void releaseReader() noexcept;

    GeometryStore* mStoreOrNull = nullptr;
    foundation::Array<Slot> mSlots;
    foundation::Array<uint32> mFreeSlots;    ///< LIFO list of reusable slots.
    foundation::Array<HierarchyNode> mNodes; ///< Hierarchy node pool: two per collider slot.
    foundation::Array<uint32> mFreeNodes;    ///< LIFO list of reusable nodes.
    uint32 mRoot = 0;                        ///< Root node, or the null node for an empty hierarchy.
    uint64 mOwner = 0;
    uint64 mEpoch = 0;
    usize mLiveCount = 0;
    uint32 mReaderCount = 0;
};
} // namespace ludus::physics::collision
