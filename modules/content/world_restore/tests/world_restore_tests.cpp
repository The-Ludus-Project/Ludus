// Stage 4 slice B: two-pass World restore.
// Covers the "Validation and performance gates" cases owned by this slice:
// two-pass forward references, save/restore, an unchanged active world after a
// failed candidate, and repeated placements resolving to distinct objects.
// See docs/architecture/object-handles-uuid.md ("Resolution and streaming").

#include <ludus/content/world_restore/world_restore.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/gameplay/world/binding_table.h>
#include <ludus/gameplay/world/entity_registry.h>

#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::world_restore;
using namespace ludus::foundation;
using ludus::foundation::identity::InstanceId;
using ludus::foundation::identity::PersistentObjectKey;
using ludus::foundation::identity::SourceObjectId;
using ludus::foundation::identity::Uuid;
using ludus::gameplay::world::BindingCandidate;
using ludus::gameplay::world::EntityId;
using ludus::gameplay::world::EntityRegistry;
using ludus::gameplay::world::ResolveStatus;
using ludus::gameplay::world::WorldBindingTable;
using WorldStatus = ludus::gameplay::world::Status;

namespace
{
// Canonical UUID text for the three objects used by the fixtures, and a distinct
// instance scope. These are fixed nonnil version-4 values.
constexpr std::string_view GUARD_UUID = "5d1f0e7a-93c2-4b8e-a4f6-1e2d3c4b5a69";
constexpr std::string_view PLAYER_UUID = "8a2b4c6d-1e3f-4a5b-9c7d-2e4f6a8b0c1d";
constexpr std::string_view SPARE_UUID = "0b7e8c1d-2f43-4a96-9d51-6c2e4f8a1b3d";

// A schema with one entity-typed property: enemy.target.
constexpr std::string_view SCHEMA = R"({
  "components": [
    { "id": "enemy", "properties": [
        { "id": "target", "type": "entity" },
        { "id": "speed", "type": "number" }
    ] }
  ]
})";

// A version-2 record whose guard entity targets the player entity, which is
// declared LATER in the array: a forward reference.
constexpr std::string_view FORWARD_RECORD = R"({
  "version": 2,
  "schema": "ludus.world-demo.v1",
  "document": "0b7e8c1d-2f43-4a96-9d51-6c2e4f8a1b3d",
  "entities": [
    { "uuid": "5d1f0e7a-93c2-4b8e-a4f6-1e2d3c4b5a69", "label": "guard-a", "name": "guard-a",
      "components": { "enemy": { "target": "8a2b4c6d-1e3f-4a5b-9c7d-2e4f6a8b0c1d", "speed": 2 } } },
    { "uuid": "8a2b4c6d-1e3f-4a5b-9c7d-2e4f6a8b0c1d", "label": "player", "name": "player",
      "components": {} }
  ]
})";

[[nodiscard]] InstanceId MakeInstance(uint8 seed) noexcept
{
    Uuid value;
    for (usize i = 0; i < identity::UUID_BYTE_COUNT; ++i)
    {
        value.Bytes[i] = static_cast<uint8>(seed + static_cast<uint8>(i));
    }
    value.Bytes[identity::UUID_VERSION_BYTE_INDEX] = static_cast<uint8>((identity::UUID_VERSION_RANDOM << 4) | 0x0A);
    value.Bytes[identity::UUID_VARIANT_BYTE_INDEX] = static_cast<uint8>(identity::UUID_STANDARD_VARIANT_BITS | 0x0B);
    return InstanceId{value};
}

[[nodiscard]] SourceObjectId ParseObject(std::string_view text) noexcept
{
    Uuid value;
    (void)identity::TryParseUuid(text.data(), text.size(), identity::UuidPolicy::RequireVersion4, value);
    return SourceObjectId{value};
}

[[nodiscard]] PersistentObjectKey KeyOf(const InstanceId& instance, std::string_view objectText) noexcept
{
    return PersistentObjectKey{ .Instance = instance, .Object = ParseObject(objectText) };
}
} // namespace

TEST_CASE("Two-pass restore resolves a forward reference", "[restore]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(16) == WorldStatus::Success);
    WorldBindingTable bindings;
    REQUIRE(bindings.TryInitialize(registry.GetCapacity()) == WorldStatus::Success);
    const InstanceId instance = MakeInstance(1);

    RestoreSummary summary;
    RestoreError error;
    REQUIRE(TryRestoreWorld(FORWARD_RECORD, SCHEMA, instance, registry, bindings, summary, error) ==
            RestoreStatus::Success);
    REQUIRE(summary.EntityCount == 2);
    REQUIRE(summary.BindingCount == 2);
    REQUIRE(summary.ResolvedLinkCount == 1);

    // Both durable keys resolve to live, distinct handles.
    EntityId guard;
    EntityId player;
    REQUIRE(bindings.TryResolve(registry, KeyOf(instance, GUARD_UUID), guard) == ResolveStatus::Resolved);
    REQUIRE(bindings.TryResolve(registry, KeyOf(instance, PLAYER_UUID), player) == ResolveStatus::Resolved);
    REQUIRE(registry.IsAlive(guard));
    REQUIRE(registry.IsAlive(player));
    REQUIRE_FALSE(guard == player);
}

TEST_CASE("A failed candidate leaves the active world unchanged", "[restore]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(16) == WorldStatus::Success);
    WorldBindingTable bindings;
    REQUIRE(bindings.TryInitialize(registry.GetCapacity()) == WorldStatus::Success);

    // Pre-existing live content that must survive a failed restore.
    EntityId existing;
    REQUIRE(registry.TryCreate(existing) == WorldStatus::Success);
    const InstanceId existingInstance = MakeInstance(9);
    const std::array<BindingCandidate, 1> existingBatch{
        BindingCandidate{ .Key = KeyOf(existingInstance, SPARE_UUID), .Entity = existing }};
    REQUIRE(bindings.TryActivate(registry, existingBatch) == WorldStatus::Success);
    const usize availableBefore = registry.GetAvailable();
    const usize bindingsBefore = bindings.GetBindingCount();
    const uint64 revisionBefore = bindings.GetBindingRevision();

    // A record whose guard targets a SourceObjectId that is not an entity of the
    // record: a dangling required link. Restore must reject it as a whole.
    constexpr std::string_view DANGLING = R"({
      "version": 2, "schema": "s", "document": "0b7e8c1d-2f43-4a96-9d51-6c2e4f8a1b3d",
      "entities": [
        { "uuid": "5d1f0e7a-93c2-4b8e-a4f6-1e2d3c4b5a69", "label": "g", "name": "g",
          "components": { "enemy": { "target": "8a2b4c6d-1e3f-4a5b-9c7d-2e4f6a8b0c1d" } } }
      ]
    })";

    RestoreSummary summary;
    RestoreError error;
    REQUIRE(TryRestoreWorld(DANGLING, SCHEMA, MakeInstance(2), registry, bindings, summary, error) ==
            RestoreStatus::UnresolvedReference);

    // Nothing changed: the staged entity was released, no binding was published.
    REQUIRE(registry.GetAvailable() == availableBefore);
    REQUIRE(bindings.GetBindingCount() == bindingsBefore);
    REQUIRE(bindings.GetBindingRevision() == revisionBefore);
    REQUIRE(registry.IsAlive(existing));
    EntityId stillThere;
    REQUIRE(bindings.TryResolve(registry, KeyOf(existingInstance, SPARE_UUID), stillThere) == ResolveStatus::Resolved);
    REQUIRE(stillThere == existing);
}

TEST_CASE("Save and restore round trip rebuilds bindings from durable keys", "[restore]")
{
    // "Save" is represented by the record text; restore must rebuild the runtime
    // bindings purely from the durable keys, never from runtime handle values.
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == WorldStatus::Success);
    WorldBindingTable bindings;
    REQUIRE(bindings.TryInitialize(registry.GetCapacity()) == WorldStatus::Success);
    const InstanceId instance = MakeInstance(3);

    RestoreSummary summary;
    RestoreError error;
    REQUIRE(TryRestoreWorld(FORWARD_RECORD, SCHEMA, instance, registry, bindings, summary, error) ==
            RestoreStatus::Success);

    EntityId guard;
    REQUIRE(bindings.TryResolve(registry, KeyOf(instance, GUARD_UUID), guard) == ResolveStatus::Resolved);
    PersistentObjectKey reverse;
    REQUIRE(bindings.TryGetBinding(guard, reverse));
    REQUIRE(reverse == KeyOf(instance, GUARD_UUID));
}

TEST_CASE("Repeated placements of one record bind to distinct scopes", "[restore]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(16) == WorldStatus::Success);
    WorldBindingTable bindings;
    REQUIRE(bindings.TryInitialize(registry.GetCapacity()) == WorldStatus::Success);

    // The same record placed under two different instance scopes resolves each
    // shared SourceObjectId to a different runtime object.
    const InstanceId placementA = MakeInstance(10);
    const InstanceId placementB = MakeInstance(20);
    RestoreSummary summary;
    RestoreError error;
    REQUIRE(TryRestoreWorld(FORWARD_RECORD, SCHEMA, placementA, registry, bindings, summary, error) ==
            RestoreStatus::Success);
    REQUIRE(TryRestoreWorld(FORWARD_RECORD, SCHEMA, placementB, registry, bindings, summary, error) ==
            RestoreStatus::Success);

    EntityId guardA;
    EntityId guardB;
    REQUIRE(bindings.TryResolve(registry, KeyOf(placementA, GUARD_UUID), guardA) == ResolveStatus::Resolved);
    REQUIRE(bindings.TryResolve(registry, KeyOf(placementB, GUARD_UUID), guardB) == ResolveStatus::Resolved);
    REQUIRE_FALSE(guardA == guardB);
}

TEST_CASE("Restore rejects a non-version-2 record and a nil instance", "[restore]")
{
    EntityRegistry registry;
    REQUIRE(registry.TryInitialize(8) == WorldStatus::Success);
    WorldBindingTable bindings;
    REQUIRE(bindings.TryInitialize(registry.GetCapacity()) == WorldStatus::Success);
    RestoreSummary summary;
    RestoreError error;

    SECTION("version 1 is not restored")
    {
        constexpr std::string_view V1 = R"({ "version": 1, "schema": "s", "entities": [] })";
        REQUIRE(TryRestoreWorld(V1, SCHEMA, MakeInstance(4), registry, bindings, summary, error) ==
                RestoreStatus::UnsupportedVersion);
    }
    SECTION("a nil instance scope is rejected")
    {
        REQUIRE(TryRestoreWorld(FORWARD_RECORD, SCHEMA, InstanceId{}, registry, bindings, summary, error) ==
                RestoreStatus::InvalidIdentity);
    }
}
