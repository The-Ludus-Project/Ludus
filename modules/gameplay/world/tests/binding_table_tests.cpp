// Stage 4 slice A: durable binding table at the World boundary.
// Covers the "Validation and performance gates" cases owned by this slice:
// rename/move preserves identity, repeated placements, cross-instance links,
// negative-cache invalidation, reserve-before-commit all-or-nothing, and
// unload/recreation of the same durable key.
// See docs/architecture/object-handles-uuid.md#resolution-and-streaming.

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/gameplay/world/binding_table.h>
#include <ludus/gameplay/world/entity_registry.h>

#include <array>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::gameplay::world;
using namespace ludus::foundation;
using ludus::foundation::identity::InstanceId;
using ludus::foundation::identity::PersistentObjectKey;
using ludus::foundation::identity::SourceObjectId;
using ludus::foundation::identity::Uuid;

namespace
{
// Build a nonnil, version-4/variant-correct UUID from a small seed. Distinct
// seeds give distinct UUIDs; the binding table only requires nonnil keys.
[[nodiscard]] Uuid MakeUuid(uint8 seed) noexcept
{
    Uuid value;
    for (usize i = 0; i < identity::UUID_BYTE_COUNT; ++i)
    {
        value.Bytes[i] = static_cast<uint8>(seed + static_cast<uint8>(i));
    }
    value.Bytes[identity::UUID_VERSION_BYTE_INDEX] = static_cast<uint8>((identity::UUID_VERSION_RANDOM << 4) | 0x0A);
    value.Bytes[identity::UUID_VARIANT_BYTE_INDEX] = static_cast<uint8>(identity::UUID_STANDARD_VARIANT_BITS | 0x0B);
    return value;
}

[[nodiscard]] InstanceId MakeInstance(uint8 seed) noexcept
{
    return InstanceId{MakeUuid(static_cast<uint8>(0x10 + seed))};
}

[[nodiscard]] SourceObjectId MakeObject(uint8 seed) noexcept
{
    return SourceObjectId{MakeUuid(static_cast<uint8>(0x80 + seed))};
}

[[nodiscard]] PersistentObjectKey MakeKey(const InstanceId& instance, const SourceObjectId& object) noexcept
{
    return PersistentObjectKey{ .Instance = instance, .Object = object };
}
} // namespace

TEST_CASE("Binding table publishes both mappings and advances the revision", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);
    REQUIRE(table.GetBindingRevision() == 0);

    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(1), MakeObject(1));
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = entity }};

    REQUIRE(table.TryActivate(registry, batch) == Status::Success);
    REQUIRE(table.GetBindingCount() == 1);
    REQUIRE(table.GetBindingRevision() == 1);

    EntityId resolved;
    REQUIRE(table.TryResolve(registry, key, resolved) == ResolveStatus::Resolved);
    REQUIRE(resolved == entity);

    PersistentObjectKey reverse;
    REQUIRE(table.TryGetBinding(entity, reverse));
    REQUIRE(reverse == key);
}

TEST_CASE("TryResolve reports the full state vocabulary", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(2), MakeObject(2));
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = entity }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);

    EntityId out;
    SECTION("Resolved for a live binding")
    {
        REQUIRE(table.TryResolve(registry, key, out) == ResolveStatus::Resolved);
    }
    SECTION("Missing for an unknown key")
    {
        const PersistentObjectKey unknown = MakeKey(MakeInstance(99), MakeObject(99));
        REQUIRE(table.TryResolve(registry, unknown, out) == ResolveStatus::Missing);
    }
    SECTION("InvalidReference for a nil key")
    {
        const PersistentObjectKey nilKey = MakeKey(InstanceId{}, MakeObject(2));
        REQUIRE(table.TryResolve(registry, nilKey, out) == ResolveStatus::InvalidReference);
    }
}

TEST_CASE("A recycled slot never inherits a former object's binding", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(2) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    EntityId first;
    REQUIRE(registry.TryCreate(first) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(3), MakeObject(3));
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = first }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);

    // Destroy the entity: detach the binding (stage-4 destruction order) then release.
    REQUIRE(table.DetachBinding(registry, first) == Status::Success);
    REQUIRE(registry.TryRelease(first) == Status::Success);

    // Recreate: the registry recycles the slot with a new generation.
    EntityId recycled;
    REQUIRE(registry.TryCreate(recycled) == Status::Success);
    REQUIRE(recycled.Slot == first.Slot);
    REQUIRE(recycled.Generation != first.Generation);

    // The recycled slot carries no binding, and the old handle resolves nothing.
    PersistentObjectKey reverse;
    REQUIRE_FALSE(table.TryGetBinding(recycled, reverse));
    EntityId out;
    REQUIRE(table.TryResolve(registry, key, out) == ResolveStatus::Missing);
}

TEST_CASE("Unload and recreation of the same key yields a fresh handle", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(4) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    EntityId first;
    REQUIRE(registry.TryCreate(first) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(4), MakeObject(4));
    const std::array<BindingCandidate, 1> firstBatch{BindingCandidate{ .Key = key, .Entity = first }};
    REQUIRE(table.TryActivate(registry, firstBatch) == Status::Success);
    const uint64 afterFirst = table.GetBindingRevision();

    // Unload: detach + release. The durable key remains available for a later resolve.
    REQUIRE(table.DetachBinding(registry, first) == Status::Success);
    REQUIRE(registry.TryRelease(first) == Status::Success);
    REQUIRE(table.GetBindingRevision() > afterFirst);

    // Recreate under the same durable key: a new incarnation, a fresh handle.
    EntityId second;
    REQUIRE(registry.TryCreate(second) == Status::Success);
    const std::array<BindingCandidate, 1> secondBatch{BindingCandidate{ .Key = key, .Entity = second }};
    REQUIRE(table.TryActivate(registry, secondBatch) == Status::Success);

    EntityId resolved;
    REQUIRE(table.TryResolve(registry, key, resolved) == ResolveStatus::Resolved);
    REQUIRE(resolved == second);
    REQUIRE_FALSE(resolved == first);
}

TEST_CASE("A cached negative result is invalidated by a binding-table revision", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(4) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    const PersistentObjectKey key = MakeKey(MakeInstance(5), MakeObject(5));

    // A caller resolves before activation: Missing, recorded against this revision.
    EntityId out;
    const uint64 cachedRevision = table.GetBindingRevision();
    REQUIRE(table.TryResolve(registry, key, out) == ResolveStatus::Missing);

    // Activate the key. The revision advances, which invalidates the cached negative.
    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = entity }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);
    REQUIRE(table.GetBindingRevision() != cachedRevision);

    // Re-resolving now succeeds.
    REQUIRE(table.TryResolve(registry, key, out) == ResolveStatus::Resolved);
    REQUIRE(out == entity);
}

TEST_CASE("Activation is all-or-nothing on a duplicate or invalid candidate", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    EntityId a;
    EntityId b;
    REQUIRE(registry.TryCreate(a) == Status::Success);
    REQUIRE(registry.TryCreate(b) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(6), MakeObject(6));

    SECTION("A repeated key within the batch publishes nothing")
    {
        const std::array<BindingCandidate, 2> batch{BindingCandidate{ .Key = key, .Entity = a },
                                                    BindingCandidate{ .Key = key, .Entity = b }};
        REQUIRE(table.TryActivate(registry, batch) == Status::InvalidConfiguration);
        REQUIRE(table.GetBindingCount() == 0);
        REQUIRE(table.GetBindingRevision() == 0);
    }
    SECTION("A dead handle in the batch publishes nothing")
    {
        REQUIRE(registry.TryRelease(b) == Status::Success);
        const PersistentObjectKey other = MakeKey(MakeInstance(6), MakeObject(7));
        const std::array<BindingCandidate, 2> batch{BindingCandidate{ .Key = key, .Entity = a },
                                                    BindingCandidate{ .Key = other, .Entity = b }};
        REQUIRE(table.TryActivate(registry, batch) == Status::InvalidEntity);
        REQUIRE(table.GetBindingCount() == 0);
        REQUIRE(table.GetBindingRevision() == 0);
    }
}

TEST_CASE("Rename or move preserves the durable key across a re-published handle", "[world][binding]")
{
    // A rename/move changes authored labels, not the SourceObjectId, so the
    // durable key is stable. Re-publishing the same key for the same live
    // incarnation resolves to the same handle.
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(4) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(7), MakeObject(1));
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = entity }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);

    EntityId resolved;
    REQUIRE(table.TryResolve(registry, key, resolved) == ResolveStatus::Resolved);
    REQUIRE(resolved == entity);
}

TEST_CASE("Repeated placements and cross-instance links resolve per scope", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(16) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    // The same prefab source object placed in two instance scopes.
    const SourceObjectId sharedObject = MakeObject(42);
    const InstanceId placementA = MakeInstance(70);
    const InstanceId placementB = MakeInstance(71);
    const PersistentObjectKey keyA = MakeKey(placementA, sharedObject);
    const PersistentObjectKey keyB = MakeKey(placementB, sharedObject);

    EntityId entityA;
    EntityId entityB;
    REQUIRE(registry.TryCreate(entityA) == Status::Success);
    REQUIRE(registry.TryCreate(entityB) == Status::Success);
    const std::array<BindingCandidate, 2> batch{BindingCandidate{ .Key = keyA, .Entity = entityA },
                                                BindingCandidate{ .Key = keyB, .Entity = entityB }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);

    // Two placements of one source resolve to two different runtime objects.
    EntityId resolvedA;
    EntityId resolvedB;
    REQUIRE(table.TryResolve(registry, keyA, resolvedA) == ResolveStatus::Resolved);
    REQUIRE(table.TryResolve(registry, keyB, resolvedB) == ResolveStatus::Resolved);
    REQUIRE(resolvedA == entityA);
    REQUIRE(resolvedB == entityB);
    REQUIRE_FALSE(resolvedA == resolvedB);

    // A cross-instance link stores the complete key of the other scope and
    // resolves across scopes.
    EntityId crossResolved;
    REQUIRE(table.TryResolve(registry, keyB, crossResolved) == ResolveStatus::Resolved);
    REQUIRE(crossResolved == entityB);
}

TEST_CASE("Reserving before commit sizes the forward map without publishing", "[world][binding]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == Status::Success);
    WorldBindingTable table;
    REQUIRE(table.TryInitialize(registry.GetCapacity()) == Status::Success);

    REQUIRE(table.TryReserve(4) == Status::Success);
    REQUIRE(table.GetBindingCount() == 0);
    REQUIRE(table.GetBindingRevision() == 0);

    EntityId entity;
    REQUIRE(registry.TryCreate(entity) == Status::Success);
    const PersistentObjectKey key = MakeKey(MakeInstance(8), MakeObject(8));
    const std::array<BindingCandidate, 1> batch{BindingCandidate{ .Key = key, .Entity = entity }};
    REQUIRE(table.TryActivate(registry, batch) == Status::Success);
    REQUIRE(table.GetBindingCount() == 1);
}
