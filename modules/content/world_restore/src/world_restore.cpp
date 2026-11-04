// Stage 4 two-pass World restore. See the header and
// docs/architecture/object-handles-uuid.md ("Resolution and streaming").

#include <ludus/content/world_restore/world_restore.h>

#include <ludus/content/identity_records/world_records.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <string_view>

namespace ludus::content::world_restore
{
using foundation::uint64;
using foundation::usize;
using foundation::identity::InstanceId;
using foundation::identity::PersistentObjectKey;
using foundation::identity::SourceObjectId;
using foundation::identity::Uuid;
using foundation::parsing::JsonDocument;
using foundation::parsing::JsonValue;
using foundation::parsing::ParseError;
using foundation::parsing::ParseStatus;
using gameplay::world::BindingCandidate;
using gameplay::world::EntityId;
using gameplay::world::EntityRegistry;
using gameplay::world::WorldBindingTable;
using identity_records::EntityReferenceProperty;

namespace
{
/// The maximum version-2 World record size mirrors the authoring
/// records' owner (content::identity_records), so restore accepts exactly what
/// the cooker can emit. Named here to avoid a magic size.
inline constexpr usize MAX_RECORD_BYTES = usize{512} * 1024;
inline constexpr uint64 WORLD_RECORD_VERSION_2 = 2;

/// Ordering over SourceObjectId by its canonical UUID bytes, for the pass-1 index.
struct SourceObjectLess final
{
    [[nodiscard]] bool operator()(const SourceObjectId& left, const SourceObjectId& right) const noexcept
    {
        for (usize i = 0; i < foundation::identity::UUID_BYTE_COUNT; ++i)
        {
            if (left.Value.Bytes[i] != right.Value.Bytes[i])
            {
                return left.Value.Bytes[i] < right.Value.Bytes[i];
            }
        }
        return false;
    }
};

/// Record a failure and return its status. `subject`, when present, names the
/// offending input; it is appended to `what` within the bounded message.
[[nodiscard]] RestoreStatus
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): diagnostic prefix and optional subject are distinct text roles.
Fail(RestoreError& outError, RestoreStatus status, std::string_view what, std::string_view subject = {}) noexcept
{
    outError.Status = status;
    content::MessageText message;
    if (subject.empty() == true)
    {
        (void)message.Set(what);
    }
    else
    {
        // Compose "<what>: <subject>" in a stack buffer, then set (truncates safely).
        char buffer[content::MAX_MESSAGE_BYTES + 1] = {};
        usize length = 0;
        const auto append = [&buffer, &length](std::string_view text) noexcept {
            for (const char character : text)
            {
                if (length >= content::MAX_MESSAGE_BYTES)
                {
                    return;
                }
                buffer[length] = character;
                ++length;
            }
        };
        append(what);
        append(": ");
        append(subject);
        (void)message.Set(std::string_view(buffer, length));
    }
    outError.Message = message;
    return status;
}

/// Parse a nonnil version-4 UUID from a JSON string value.
[[nodiscard]] bool TryReadSourceObject(JsonValue value, SourceObjectId& outValue) noexcept
{
    std::string_view text;
    if (value.String(text) == false)
    {
        return false;
    }
    Uuid parsed;
    if (foundation::identity::TryParseUuid(text.data(),
                                           text.size(),
                                           foundation::identity::UuidPolicy::RequireVersion4,
                                           parsed) != foundation::identity::UuidStatus::Success)
    {
        return false;
    }
    outValue = SourceObjectId{parsed};
    return true;
}

[[nodiscard]] bool IsReference(std::span<const EntityReferenceProperty> references,
                               std::string_view component,
                               std::string_view property) noexcept
{
    for (const EntityReferenceProperty& reference : references)
    {
        if (reference.Component.View() == component && reference.Property.View() == property)
        {
            return true;
        }
    }
    return false;
}

/// One entity-typed link named in a record entity: the owner's row index in the
/// candidate list and the target's SourceObjectId. Resolved in pass 2.
struct PendingLink final
{
    usize OwnerIndex = 0;
    SourceObjectId Target;
};
} // namespace

const char* GetRestoreStatusName(RestoreStatus status) noexcept
{
    switch (status)
    {
        case RestoreStatus::Success:
            return "Success";
        case RestoreStatus::Malformed:
            return "Malformed";
        case RestoreStatus::UnsupportedVersion:
            return "UnsupportedVersion";
        case RestoreStatus::InvalidSchema:
            return "InvalidSchema";
        case RestoreStatus::InvalidIdentity:
            return "InvalidIdentity";
        case RestoreStatus::UnresolvedReference:
            return "UnresolvedReference";
        case RestoreStatus::CapacityExceeded:
            return "CapacityExceeded";
        case RestoreStatus::OutOfMemory:
            return "OutOfMemory";
    }
    return "Unknown";
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): API inputs are distinct World and schema JSON documents.
RestoreStatus TryRestoreWorld(std::string_view recordJson,
                              std::string_view schemaJson,
                              const InstanceId& instance,
                              EntityRegistry& inoutRegistry,
                              WorldBindingTable& inoutBindings,
                              RestoreSummary& outSummary,
                              RestoreError& outError) noexcept
{
    outError = {};
    if (instance.IsNil() == true)
    {
        return Fail(outError, RestoreStatus::InvalidIdentity, "instance scope must be a nonnil identity");
    }
    if (recordJson.size() > MAX_RECORD_BYTES)
    {
        return Fail(outError, RestoreStatus::Malformed, "World record is too large");
    }

    // Read the schema to learn which component properties are entity references.
    foundation::Array<EntityReferenceProperty> references;
    identity_records::IdentityRecordError schemaError;
    if (identity_records::ReadEntityReferenceProperties(schemaJson, references, schemaError) !=
        identity_records::IdentityRecordStatus::Success)
    {
        return Fail(outError, RestoreStatus::InvalidSchema, schemaError.Message.View());
    }

    JsonDocument document;
    ParseError parseError;
    foundation::parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_RECORD_BYTES;
    if (document.Read(recordJson, parseError, limits) != ParseStatus::Ok)
    {
        return Fail(outError, RestoreStatus::Malformed, "World record is not valid JSON");
    }
    const JsonValue root = document.Root();
    uint64 version = 0;
    if (root.Object() == false || root.Get("version").Integer(version) == false)
    {
        return Fail(outError, RestoreStatus::Malformed, "World record has no version");
    }
    if (version != WORLD_RECORD_VERSION_2)
    {
        return Fail(outError, RestoreStatus::UnsupportedVersion, "only version-2 World records are restored");
    }
    const JsonValue entities = root.Get("entities");
    if (entities.Array() == false)
    {
        return Fail(outError, RestoreStatus::Malformed, "World record has no entities array");
    }
    const usize entityCount = entities.Count();

    // Reserve staging capacity up front so a partial restore cannot strand state.
    using ObjectIndex = foundation::core::SortedMap<SourceObjectId, EntityId, SourceObjectLess>;
    foundation::Array<SourceObjectId> objects;
    foundation::Array<EntityId> created;
    ObjectIndex index;
    foundation::Array<PendingLink> links;
    if (objects.TryResize(entityCount) == false || created.TryResize(entityCount) == false ||
        index.TryEnsureCapacity(entityCount) == false)
    {
        return Fail(outError, RestoreStatus::OutOfMemory, "cannot stage the restore candidate");
    }
    if (inoutRegistry.CheckStructuralBudget(entityCount, 0) != gameplay::world::Status::Success)
    {
        return Fail(outError, RestoreStatus::CapacityExceeded, "registry has no room for the record entities");
    }

    // --- Pass 1: create every candidate entity and record its durable key. ---
    usize createdCount = 0;
    const auto rollback = [&inoutRegistry, &created, &createdCount]() noexcept {
        for (usize i = 0; i < createdCount; ++i)
        {
            (void)inoutRegistry.TryRelease(created[i]);
        }
    };

    for (usize i = 0; i < entityCount; ++i)
    {
        const JsonValue entity = entities.At(i);
        SourceObjectId object;
        if (entity.Object() == false || TryReadSourceObject(entity.Get("uuid"), object) == false)
        {
            rollback();
            return Fail(outError, RestoreStatus::InvalidIdentity, "entity has no nonnil version-4 uuid");
        }
        const ObjectIndex::InsertResult insertion = index.TryAddInPlace(object, EntityId{});
        if (insertion.Value == nullptr)
        {
            rollback();
            return Fail(outError, RestoreStatus::OutOfMemory, "cannot index the candidate entities");
        }
        if (insertion.Inserted == false)
        {
            rollback();
            return Fail(outError, RestoreStatus::InvalidIdentity, "two entities share one uuid");
        }
        EntityId entityId;
        if (inoutRegistry.TryCreate(entityId) != gameplay::world::Status::Success)
        {
            rollback();
            return Fail(outError, RestoreStatus::CapacityExceeded, "cannot create a record entity");
        }
        *insertion.Value = entityId;
        objects[i] = object;
        created[i] = entityId;
        ++createdCount;

        // Collect the entity-typed links of this entity for pass 2. A link whose
        // target appears later in the record is still resolvable because pass 2
        // runs after every candidate exists.
        const JsonValue components = entity.Get("components");
        for (usize c = 0; c < components.MemberCount(); ++c)
        {
            std::string_view componentId;
            JsonValue component;
            if (components.MemberAt(c, componentId, component) == false || component.Object() == false)
            {
                continue;
            }
            for (usize p = 0; p < component.MemberCount(); ++p)
            {
                std::string_view propertyId;
                JsonValue property;
                if (component.MemberAt(p, propertyId, property) == false)
                {
                    continue;
                }
                if (IsReference(references, componentId, propertyId) == false)
                {
                    continue;
                }
                std::string_view target;
                if (property.String(target) == false)
                {
                    rollback();
                    return Fail(outError, RestoreStatus::Malformed, "entity reference must be a string", propertyId);
                }
                if (target.empty() == true)
                {
                    // An empty reference is an explicit "no target"; not a link.
                    continue;
                }
                SourceObjectId linkTarget;
                if (TryReadSourceObject(property, linkTarget) == false)
                {
                    rollback();
                    return Fail(outError, RestoreStatus::InvalidIdentity, "entity reference is not a version-4 uuid");
                }
                if (links.TryAdd(PendingLink{ .OwnerIndex = i, .Target = linkTarget }) == false)
                {
                    rollback();
                    return Fail(outError, RestoreStatus::OutOfMemory, "cannot stage the required links");
                }
            }
        }
    }

    // --- Pass 2: resolve every required local link now that all entities exist. ---
    usize resolvedLinkCount = 0;
    for (const PendingLink& link : links)
    {
        if (index.Find(link.Target) == nullptr)
        {
            rollback();
            return Fail(outError,
                        RestoreStatus::UnresolvedReference,
                        "entity reference names no entity of this record");
        }
        ++resolvedLinkCount;
    }

    // Build the binding candidates from the instance scope and durable keys.
    foundation::Array<BindingCandidate> candidates;
    if (candidates.TryResize(entityCount) == false)
    {
        rollback();
        return Fail(outError, RestoreStatus::OutOfMemory, "cannot stage the binding candidates");
    }
    for (usize i = 0; i < entityCount; ++i)
    {
        candidates[i] = BindingCandidate
        {
            .Key = PersistentObjectKey{ .Instance = instance, .Object = objects[i] },
            .Entity = created[i],
        };
    }

    // Publish all durable bindings in one atomic step. On failure nothing is
    // published and the staged entities are released, leaving the world unchanged.
    if (inoutBindings.TryReserve(entityCount) != gameplay::world::Status::Success)
    {
        rollback();
        return Fail(outError, RestoreStatus::CapacityExceeded, "binding table cannot reserve capacity");
    }
    const gameplay::world::Status activated =
        inoutBindings.TryActivate(inoutRegistry, std::span<const BindingCandidate>(candidates.GetData(), entityCount));
    if (activated != gameplay::world::Status::Success)
    {
        rollback();
        return Fail(outError, RestoreStatus::CapacityExceeded, "binding table rejected the candidate");
    }

    outSummary = RestoreSummary
    {
        .EntityCount = createdCount,
        .BindingCount = entityCount,
        .ResolvedLinkCount = resolvedLinkCount,
    };
    return RestoreStatus::Success;
}
} // namespace ludus::content::world_restore
