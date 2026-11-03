#pragma once

// Stage 4 of the object-handles/UUID architecture: durable binding at the World
// boundary. See docs/architecture/object-handles-uuid.md, sections "Resolution
// and streaming" (the two derived mappings, TryResolve states, reserve-before-
// commit and the negative-cache revision rule) and "Authoring and prefab
// instances" (instance scopes and PersistentObjectKey formation). The
// reverse-binding owner/generation validation that prevents a recycled slot
// from inheriting a former object's UUID credits Brian Hawkins, "Pointer
// Patching Assets", Game Programming Gems 3, article 1.5, pp. 44-48 (design
// inspiration only; no sample code is copied). Review:
// docs/architecture/object-handles-uuid-reference-review.md.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/gameplay/world/entity_id.h>
#include <ludus/gameplay/world/entity_registry.h>

#include <span>

namespace ludus::gameplay::world
{
/// @brief Outcome of resolving a durable key through a WorldBindingTable.
///
/// TryResolve() performs no I/O and no implicit loading; it reports how the key
/// relates to the bindings this table currently holds and, for Resolved, to the
/// live incarnation in the owning registry. See the "Resolution and streaming"
/// section of docs/architecture/object-handles-uuid.md.
enum class ResolveStatus : foundation::uint8
{
    /// A matching binding exists and its runtime incarnation is still live.
    /// Check activity and any required capability before use.
    Resolved,
    /// An authoritative catalog/save index knows the key, but its instance is not
    /// loaded (no binding is resident). The caller may explicitly request streaming.
    NotResident,
    /// No binding and no authoritative record for the key in the available scope.
    /// Report it; this is not proof of permanent deletion.
    Missing,
    /// A binding exists but the required asset kind or gameplay capability does not match.
    TypeMismatch,
    /// The key is malformed, names an unknown scope, or is a duplicate identity.
    /// A required binding with this status is rejected.
    InvalidReference,
    /// Two live bindings claim the same durable key; the table rejects the conflict.
    Conflict,
};

/// Return a stable, human-readable name for `status` (for diagnostics and logs).
/// The returned pointer is a static string literal and is never null.
[[nodiscard]] const char* GetResolveStatusName(ResolveStatus status) noexcept;

/// @brief Strict weak ordering over PersistentObjectKey for the sorted binding table.
///
/// Orders lexicographically by the Instance UUID bytes, then the Object UUID
/// bytes, so the ordering is total and independent of host endianness. The
/// comparator is empty and nonthrowing, as FoundationContainers SortedMap
/// requires. It lives at the World boundary rather than in the identity value
/// header, which stays free of ordering policy.
struct PersistentKeyLess final
{
    /// True when `left` orders before `right` by (Instance bytes, then Object bytes).
    [[nodiscard]] bool operator()(const foundation::identity::PersistentObjectKey& left,
                                  const foundation::identity::PersistentObjectKey& right) const noexcept;
};

/// @brief One candidate durable binding published when a world activates.
///
/// The caller forms the PersistentObjectKey from the activating instance scope's
/// InstanceId and each authored SourceObjectId, and supplies the runtime EntityId
/// the owning registry issued for that object. Transient entities without durable
/// identity are simply omitted; they never enter the table.
struct BindingCandidate final
{
    /// Durable key `(InstanceId, SourceObjectId)` for this object.
    foundation::identity::PersistentObjectKey Key;
    /// Runtime handle the owning registry issued for the object.
    EntityId Entity;
};

/// @brief Durable-key resolution layer owned at the World boundary.
///
/// Holds the two derived mappings the architecture requires, published and
/// removed together with object activation and destruction:
///
/// 1. `PersistentObjectKey -> EntityId` (durable key to runtime handle), and
/// 2. runtime slot -> optional durable binding, validated by owner and
///    generation so a recycled slot never inherits a former object's UUID.
///
/// Neither mapping is a second source of truth: the authored records and the
/// registry remain authoritative, and these mappings are rebuilt rather than
/// authored. The table advances a monotonic revision on every publish/removal so
/// a cached negative result is invalidated once the bindings change.
///
/// The initial implementation is a sorted contiguous table (SortedMap) with
/// exact key equality, suitable for small immutable candidate worlds. Capacity is
/// reserved before a batch commits; a reservation failure leaves the table and
/// the active world unchanged. Structural mutation is owner-thread only, like the
/// registry it serves. The table borrows the registry to validate incarnations
/// and never owns or destroys entities.
class WorldBindingTable final
{
public:
    /// Empty table that owns no storage; size it with TryInitialize() before use.
    WorldBindingTable() noexcept = default;
    /// Copying is disabled: the table is the single owner of its world's bindings.
    WorldBindingTable(const WorldBindingTable&) = delete;
    /// Copying is disabled: the table is the single owner of its world's bindings.
    WorldBindingTable& operator=(const WorldBindingTable&) = delete;
    /// Transfer the mappings and revision; the source becomes empty.
    WorldBindingTable(WorldBindingTable&& other) noexcept = default;
    /// Transfer the mappings and revision; the source becomes empty.
    WorldBindingTable& operator=(WorldBindingTable&& other) noexcept = default;
    /// Release both mappings.
    ~WorldBindingTable() = default;

    /// @brief Size the slot-indexed reverse mapping to match a registry's capacity.
    ///
    /// Call once before activation, with the capacity the owning registry was
    /// initialized to. Transactional: on allocation failure the table is left
    /// empty and Status::AllocationFailure is returned.
    /// @param slotCapacity The owning registry's slot capacity (GetCapacity()).
    /// @return Success, or AllocationFailure when the reverse array cannot be sized.
    [[nodiscard]] Status TryInitialize(foundation::uint32 slotCapacity) noexcept;

    /// @brief Reserve capacity for `additional` new bindings before a commit.
    ///
    /// Reserves forward-map capacity through the allocation-failure-capable seam so
    /// a later TryActivate() of the same batch cannot fail partway. Never shrinks.
    /// @param additional Number of bindings the next batch will publish.
    /// @return Success, or AllocationFailure when capacity cannot be reserved.
    [[nodiscard]] Status TryReserve(foundation::usize additional) noexcept;

    /// @brief Publish a batch of durable bindings as one all-or-nothing step.
    ///
    /// Validates every candidate against the registry (owner match and live
    /// incarnation) and against the existing bindings, reserves capacity, then
    /// publishes both mappings and advances the revision once. If any candidate is
    /// invalid or duplicates an existing key, nothing is published and the active
    /// world is unchanged.
    /// @param registry The owning registry that issued the candidate handles.
    /// @param candidates The bindings to publish; keys must be unique within the batch.
    /// @return Success; InvalidEntity for a dead/foreign handle; InvalidConfiguration
    /// for a nil key, an out-of-range slot or a duplicate key; AllocationFailure when
    /// capacity cannot be reserved.
    [[nodiscard]] Status TryActivate(const EntityRegistry& registry,
                                     std::span<const BindingCandidate> candidates) noexcept;

    /// @brief Detach the durable binding of one entity before its slot is recycled.
    ///
    /// Call inside the registry's destruction order, after access is closed and
    /// before the slot is released, so the recycled slot carries no stale binding.
    /// Removes both mappings for the entity and advances the revision. Detaching an
    /// entity that holds no binding is a no-op that still succeeds.
    /// @param registry The owning registry (its world token identifies ownership).
    /// @param entity The entity whose durable binding is being detached.
    /// @return Success, or InvalidEntity when `entity` belongs to another registry.
    [[nodiscard]] Status DetachBinding(const EntityRegistry& registry, EntityId entity) noexcept;

    /// @brief Resolve a durable key to a live runtime handle. No I/O, no loading.
    ///
    /// Looks the key up in the forward mapping and, on a hit, validates the stored
    /// handle against the registry (owner and generation). A binding whose
    /// incarnation is gone is removed and reported as Missing. The output is written
    /// only when the result is Resolved; it is left unchanged otherwise.
    /// @param registry The owning registry used to validate the incarnation.
    /// @param key The durable key to resolve.
    /// @param outEntity Receives the live handle on Resolved; unchanged otherwise.
    /// @return A ResolveStatus describing the key/binding relationship.
    [[nodiscard]] ResolveStatus TryResolve(const EntityRegistry& registry,
                                           const foundation::identity::PersistentObjectKey& key,
                                           EntityId& outEntity) noexcept;

    /// @brief The reverse binding recorded for a runtime slot, if any.
    ///
    /// Returns the durable key bound to `entity`'s slot only when the recorded
    /// owner and generation match `entity` exactly, so a recycled slot never
    /// reports a former object's key. Writes `outKey` only when it returns true.
    /// @param entity The runtime handle whose durable binding is queried.
    /// @param outKey Receives the durable key when a matching binding exists.
    /// @return True when a matching durable binding exists for the entity.
    [[nodiscard]] bool TryGetBinding(EntityId entity, foundation::identity::PersistentObjectKey& outKey) const noexcept;

    /// Number of live durable bindings currently published.
    [[nodiscard]] foundation::usize GetBindingCount() const noexcept
    {
        return mForward.GetSize();
    }

    /// @brief Monotonic revision advanced on every publish or removal.
    ///
    /// A cached resolution result (including a negative Missing/NotResident result)
    /// is stale once this value changes, so callers invalidate such caches by
    /// comparing the revision they recorded. See the "Resolution and streaming"
    /// section of docs/architecture/object-handles-uuid.md.
    [[nodiscard]] foundation::uint64 GetBindingRevision() const noexcept
    {
        return mBindingRevision;
    }

private:
    /// Reverse mapping entry: the durable key bound to a slot, plus the owner and
    /// generation it was bound under, so a recycled slot is never misread.
    struct SlotBinding final
    {
        /// Durable key bound to this slot; meaningful only when HasBinding is true.
        foundation::identity::PersistentObjectKey Key;
        /// World/owner token of the handle that was bound; zero when unbound.
        foundation::uint64 Owner = 0;
        /// Generation of the handle that was bound; zero when unbound.
        foundation::uint32 Generation = 0;
        /// True when this slot currently carries a durable binding.
        bool HasBinding = false;
    };

    foundation::core::SortedMap<foundation::identity::PersistentObjectKey, EntityId, PersistentKeyLess> mForward;
    foundation::core::Array<SlotBinding> mReverse;
    foundation::uint64 mBindingRevision = 0;
};
} // namespace ludus::gameplay::world
