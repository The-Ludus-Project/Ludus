#include <ludus/content/identity_records/world_records.h>

#include "internal/record_json.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/parsing/json.hpp>

namespace ludus::content::identity_records
{
namespace
{
using foundation::parsing::JsonValue;
using internal::Fail;

/// First output capacity tried for a version-2 document, as a multiple of the input size.
constexpr usize INITIAL_OUTPUT_EXPANSION = 4;
/// Fixed output headroom for the envelope and short inputs.
constexpr usize OUTPUT_HEADROOM_BYTES = usize{64} * 1024;
/// Largest version-2 document the replay writes; capacity doubles up to this bound.
constexpr usize MAX_OUTPUT_BYTES = usize{16} * 1024 * 1024;

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

/// Validated replay inputs shared by every emission attempt.
struct Replay final
{
    const MigrationManifest& Manifest;
    const MigrationDocument& Document;
    std::span<const EntityReferenceProperty> References;
    std::string_view Schema;
    JsonValue Entities;
};

/// Write `components`, replacing entity references by their target UUIDs.
[[nodiscard]] IdentityRecordStatus WriteComponents(const Replay& replay,
                                                   JsonValue components,
                                                   internal::IndentedWriter& inoutWriter,
                                                   IdentityRecordError& outError) noexcept
{
    inoutWriter.Open('{');
    for (usize i = 0; i < components.MemberCount(); ++i)
    {
        std::string_view componentId;
        JsonValue component;
        [[maybe_unused]] const bool isMember = components.MemberAt(i, componentId, component);
        LUDUS_ASSERT(isMember == true);
        inoutWriter.BeginMember(componentId);
        if (component.Object() == false)
        {
            inoutWriter.WriteValue(component);
            continue;
        }
        inoutWriter.Open('{');
        for (usize j = 0; j < component.MemberCount(); ++j)
        {
            std::string_view propertyId;
            JsonValue property;
            [[maybe_unused]] const bool isProperty = component.MemberAt(j, propertyId, property);
            LUDUS_ASSERT(isProperty == true);
            inoutWriter.BeginMember(propertyId);
            if (IsReference(replay.References, componentId, propertyId) == false)
            {
                inoutWriter.WriteValue(property);
                continue;
            }
            std::string_view target;
            if (property.String(target) == false)
            {
                return Fail(outError, IdentityRecordStatus::Malformed, "entity reference must be a string", propertyId);
            }
            if (target.empty() == true)
            {
                // An empty reference means "no target" in version 1 and stays empty.
                inoutWriter.WriteString(target);
                continue;
            }
            const MigrationObject* const targetOrNull = replay.Manifest.FindObject(replay.Document, target);
            if (targetOrNull == nullptr)
            {
                return Fail(outError,
                            IdentityRecordStatus::UnresolvedReference,
                            "entity reference names no object of this document",
                            target);
            }
            inoutWriter.WriteUuid(targetOrNull->Object.Value);
        }
        inoutWriter.Close('}');
    }
    inoutWriter.Close('}');
    return IdentityRecordStatus::Success;
}

[[nodiscard]] IdentityRecordStatus
EmitVersion2(const Replay& replay, usize capacity, Bytes& outJson, IdentityRecordError& outError) noexcept
{
    internal::IndentedWriter writer(capacity);
    writer.Open('{');
    writer.BeginMember("version");
    writer.WriteInteger(WORLD_RECORD_VERSION_2);
    writer.BeginMember("schema");
    writer.WriteString(replay.Schema);
    writer.BeginMember("document");
    writer.WriteUuid(replay.Document.Document.Value);
    writer.BeginMember("entities");
    writer.Open('[');
    for (usize i = 0; i < replay.Entities.Count(); ++i)
    {
        const JsonValue entity = replay.Entities.At(i);
        std::string_view legacyId;
        [[maybe_unused]] const bool isString = entity.Get("id").String(legacyId);
        LUDUS_ASSERT(isString == true);
        const MigrationObject* const objectOrNull = replay.Manifest.FindObject(replay.Document, legacyId);
        LUDUS_ASSERT(objectOrNull != nullptr);
        writer.BeginElement();
        writer.Open('{');
        writer.BeginMember("uuid");
        writer.WriteUuid(objectOrNull->Object.Value);
        writer.BeginMember("label");
        writer.WriteString(legacyId);
        writer.BeginMember("name");
        writer.WriteValue(entity.Get("name"));
        writer.BeginMember("components");
        const IdentityRecordStatus status = WriteComponents(replay, entity.Get("components"), writer, outError);
        if (status != IdentityRecordStatus::Success)
        {
            return status;
        }
        writer.Close('}');
    }
    writer.Close(']');
    writer.Close('}');
    const IdentityRecordStatus finished = writer.Finish(outJson);
    if (finished != IdentityRecordStatus::Success)
    {
        return Fail(outError, finished, "cannot encode the version-2 World record");
    }
    return IdentityRecordStatus::Success;
}
} // namespace

IdentityRecordStatus ReadEntityReferenceProperties(std::string_view schemaJson,
                                                   foundation::Array<EntityReferenceProperty>& outProperties,
                                                   IdentityRecordError& outError) noexcept
{
    outError = {};
    if (schemaJson.size() > MAX_WORLD_SCHEMA_BYTES)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World schema is too large");
    }
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    if (document.Read(schemaJson, parseError, internal::MakeLimits(MAX_WORLD_SCHEMA_BYTES)) !=
        foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World schema is not valid JSON");
    }
    const JsonValue components = document.Root().Get("components");
    if (components.Array() == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World schema has no components array");
    }
    foundation::Array<EntityReferenceProperty> candidate;
    for (usize i = 0; i < components.Count(); ++i)
    {
        const JsonValue component = components.At(i);
        const JsonValue properties = component.Get("properties");
        std::string_view componentId;
        if (component.Get("id").String(componentId) == false || properties.Array() == false)
        {
            return Fail(outError, IdentityRecordStatus::Malformed, "schema component needs id and properties");
        }
        for (usize j = 0; j < properties.Count(); ++j)
        {
            const JsonValue property = properties.At(j);
            std::string_view propertyId;
            std::string_view type;
            if (property.Get("id").String(propertyId) == false || property.Get("type").String(type) == false)
            {
                return Fail(outError,
                            IdentityRecordStatus::Malformed,
                            "schema property needs id and type",
                            componentId);
            }
            if (type != "entity")
            {
                continue;
            }
            EntityReferenceProperty reference;
            if (reference.Component.Set(componentId) == false || reference.Property.Set(propertyId) == false)
            {
                return Fail(outError, IdentityRecordStatus::Limit, "schema ID is too long", propertyId);
            }
            if (candidate.TryAdd(reference) == false)
            {
                return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the reference table");
            }
        }
    }
    outProperties = Move(candidate);
    return IdentityRecordStatus::Success;
}

IdentityRecordStatus ApplyWorldMigration(const MigrationManifest& manifest,
                                         const MigrationSource& source,
                                         std::span<const EntityReferenceProperty> references,
                                         Bytes& outWorldJson,
                                         IdentityRecordError& outError) noexcept
{
    outError = {};
    const std::string_view documentPath = source.Path;
    const std::string_view worldJson = source.WorldJson;
    if (worldJson.size() > MAX_WORLD_DOCUMENT_BYTES)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World document is too large", documentPath);
    }
    const MigrationDocument* const recordOrNull = manifest.FindDocument(documentPath);
    if (recordOrNull == nullptr)
    {
        return Fail(outError,
                    IdentityRecordStatus::UnknownDocument,
                    "manifest has no record for the document",
                    documentPath);
    }
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    if (document.Read(worldJson, parseError, internal::MakeLimits(MAX_WORLD_DOCUMENT_BYTES)) !=
        foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World document is not valid JSON", documentPath);
    }
    const JsonValue root = document.Root();
    uint64 version = 0;
    if (root.Object() == false || root.Get("version").Integer(version) == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World document has no version", documentPath);
    }
    if (version != WORLD_RECORD_VERSION_1)
    {
        return Fail(outError,
                    IdentityRecordStatus::UnsupportedVersion,
                    "only version-1 World documents are migrated",
                    documentPath);
    }
    std::string_view schema;
    const JsonValue entities = root.Get("entities");
    if (root.Fields({"version", "schema", "entities"}) == false || root.Get("schema").String(schema) == false ||
        entities.Array() == false)
    {
        return Fail(outError,
                    IdentityRecordStatus::Unsupported,
                    "World root must have exactly version, schema and entities",
                    documentPath);
    }
    if (entities.Count() > MAX_DOCUMENT_OBJECTS)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World document has too many entities", documentPath);
    }

    // Every World entity must match exactly one manifest object, and every object one entity.
    const std::span<const MigrationObject> objects = manifest.GetObjects(*recordOrNull);
    foundation::StaticArray<bool, MAX_DOCUMENT_OBJECTS> isMatched{};
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const JsonValue entity = entities.At(i);
        std::string_view legacyId;
        if (entity.Object() == false || entity.Fields({"id", "name", "components"}) == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::Unsupported,
                        "World entity must have exactly id, name and components",
                        documentPath);
        }
        if (entity.Get("components").Object() == false)
        {
            return Fail(outError, IdentityRecordStatus::Malformed, "entity components must be an object", documentPath);
        }
        if (entity.Get("id").String(legacyId) == false || IsLegacyId(legacyId) == false)
        {
            return Fail(outError, IdentityRecordStatus::InvalidLegacyId, "invalid entity id", documentPath);
        }
        const MigrationObject* const objectOrNull = manifest.FindObject(*recordOrNull, legacyId);
        if (objectOrNull == nullptr)
        {
            return Fail(outError, IdentityRecordStatus::StaleManifest, "World entity has no committed UUID", legacyId);
        }
        const usize index = static_cast<usize>(objectOrNull - objects.data());
        if (isMatched[index] == true)
        {
            return Fail(outError,
                        IdentityRecordStatus::InvalidLegacyId,
                        "entity id repeated in the document",
                        legacyId);
        }
        isMatched[index] = true;
    }
    for (usize i = 0; i < objects.size(); ++i)
    {
        if (isMatched[i] == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::StaleManifest,
                        "manifest object is missing from the World",
                        objects[i].LegacyId.View());
        }
    }

    const Replay replay
    {
        .Manifest = manifest,
        .Document = *recordOrNull,
        .References = references,
        .Schema = schema,
        .Entities = entities,
    };
    usize capacity = worldJson.size() * INITIAL_OUTPUT_EXPANSION + OUTPUT_HEADROOM_BYTES;
    while (true)
    {
        Bytes candidate;
        IdentityRecordError attemptError;
        const IdentityRecordStatus status = EmitVersion2(replay, capacity, candidate, attemptError);
        if (status == IdentityRecordStatus::Success)
        {
            outWorldJson = Move(candidate);
            return status;
        }
        if (status != IdentityRecordStatus::Limit || capacity >= MAX_OUTPUT_BYTES)
        {
            outError = attemptError;
            return status;
        }
        capacity = capacity * 2 < MAX_OUTPUT_BYTES ? capacity * 2 : MAX_OUTPUT_BYTES;
    }
}

namespace
{
using foundation::identity::SourceObjectId;
using foundation::identity::Uuid;

/// One entity selected for duplication: its JSON node and its minted new UUID.
struct DuplicationClone final
{
    JsonValue Entity;
    Uuid OldUuid;
    Uuid NewUuid;
};

/// Return the clone whose source UUID equals `target`, or null when `target` is
/// outside the copied set (a deliberate external reference).
[[nodiscard]] const DuplicationClone* FindCloneByOldUuid(std::span<const DuplicationClone> clones,
                                                         const Uuid& target) noexcept
{
    for (const DuplicationClone& clone : clones)
    {
        if (clone.OldUuid == target)
        {
            return &clone;
        }
    }
    return nullptr;
}

/// Mint a fresh UUID not present in `taken`, retrying up to the shared bound.
[[nodiscard]] IdentityRecordStatus MintUnusedUuid(const foundation::identity::EntropySource& entropy,
                                                  const foundation::Array<Uuid>& taken,
                                                  Uuid& outValue,
                                                  IdentityRecordError& outError) noexcept
{
    for (usize attempt = 0; attempt < MAX_UUID_GENERATION_ATTEMPTS; ++attempt)
    {
        Uuid candidate;
        if (foundation::identity::TryGenerateUuidV4(entropy, candidate) != foundation::identity::UuidStatus::Success)
        {
            return Fail(outError,
                        IdentityRecordStatus::EntropyUnavailable,
                        "platform entropy is unavailable; no identity was minted");
        }
        bool isTaken = false;
        for (const Uuid& existing : taken)
        {
            if (existing == candidate)
            {
                isTaken = true;
                break;
            }
        }
        if (isTaken == false)
        {
            outValue = candidate;
            return IdentityRecordStatus::Success;
        }
    }
    return Fail(outError, IdentityRecordStatus::IdentityConflict, "every generated UUID candidate was already in use");
}

/// Write one cloned entity, rewriting internal entity references to new UUIDs.
[[nodiscard]] IdentityRecordStatus WriteClone(const DuplicationClone& clone,
                                              std::span<const DuplicationClone> clones,
                                              std::span<const EntityReferenceProperty> references,
                                              internal::IndentedWriter& inoutWriter,
                                              IdentityRecordError& outError) noexcept
{
    const JsonValue entity = clone.Entity;
    inoutWriter.BeginElement();
    inoutWriter.Open('{');
    inoutWriter.BeginMember("uuid");
    inoutWriter.WriteUuid(clone.NewUuid);
    inoutWriter.BeginMember("label");
    inoutWriter.WriteValue(entity.Get("label"));
    inoutWriter.BeginMember("name");
    inoutWriter.WriteValue(entity.Get("name"));
    inoutWriter.BeginMember("components");
    const JsonValue components = entity.Get("components");
    inoutWriter.Open('{');
    for (usize i = 0; i < components.MemberCount(); ++i)
    {
        std::string_view componentId;
        JsonValue component;
        [[maybe_unused]] const bool isMember = components.MemberAt(i, componentId, component);
        LUDUS_ASSERT(isMember == true);
        inoutWriter.BeginMember(componentId);
        if (component.Object() == false)
        {
            inoutWriter.WriteValue(component);
            continue;
        }
        inoutWriter.Open('{');
        for (usize j = 0; j < component.MemberCount(); ++j)
        {
            std::string_view propertyId;
            JsonValue property;
            [[maybe_unused]] const bool isProperty = component.MemberAt(j, propertyId, property);
            LUDUS_ASSERT(isProperty == true);
            inoutWriter.BeginMember(propertyId);
            if (IsReference(references, componentId, propertyId) == false)
            {
                inoutWriter.WriteValue(property);
                continue;
            }
            std::string_view text;
            if (property.String(text) == false)
            {
                return Fail(outError, IdentityRecordStatus::Malformed, "entity reference must be a string", propertyId);
            }
            if (text.empty() == true)
            {
                inoutWriter.WriteString(text);
                continue;
            }
            Uuid target;
            if (internal::TryReadIdentity(property, target) == false)
            {
                return Fail(outError, IdentityRecordStatus::InvalidIdentity, "entity reference is not a v4 uuid", text);
            }
            // A reference into the copied set points the clone at the matching
            // clone; a reference outside it is retained as a deliberate external link.
            const DuplicationClone* const targetOrNull = FindCloneByOldUuid(clones, target);
            if (targetOrNull == nullptr)
            {
                inoutWriter.WriteUuid(target);
            }
            else
            {
                inoutWriter.WriteUuid(targetOrNull->NewUuid);
            }
        }
        inoutWriter.Close('}');
    }
    inoutWriter.Close('}');
    inoutWriter.Close('}');
    return IdentityRecordStatus::Success;
}

/// Emit the version-2 record with the original entities followed by the clones.
[[nodiscard]] IdentityRecordStatus EmitDuplicated(JsonValue root,
                                                  std::span<const DuplicationClone> clones,
                                                  std::span<const EntityReferenceProperty> references,
                                                  usize capacity,
                                                  Bytes& outJson,
                                                  IdentityRecordError& outError) noexcept
{
    const JsonValue entities = root.Get("entities");
    internal::IndentedWriter writer(capacity);
    writer.Open('{');
    writer.BeginMember("version");
    writer.WriteInteger(WORLD_RECORD_VERSION_2);
    writer.BeginMember("schema");
    writer.WriteValue(root.Get("schema"));
    writer.BeginMember("document");
    writer.WriteValue(root.Get("document"));
    writer.BeginMember("entities");
    writer.Open('[');
    // Preserve every original entity verbatim.
    for (usize i = 0; i < entities.Count(); ++i)
    {
        writer.BeginElement();
        writer.WriteValue(entities.At(i));
    }
    // Append the clones with rewritten internal references.
    for (const DuplicationClone& clone : clones)
    {
        const IdentityRecordStatus status = WriteClone(clone, clones, references, writer, outError);
        if (status != IdentityRecordStatus::Success)
        {
            return status;
        }
    }
    writer.Close(']');
    writer.Close('}');
    const IdentityRecordStatus finished = writer.Finish(outJson);
    if (finished != IdentityRecordStatus::Success)
    {
        return Fail(outError, finished, "cannot encode the duplicated World record");
    }
    return IdentityRecordStatus::Success;
}
} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): API inputs are distinct World and schema JSON documents.
IdentityRecordStatus DuplicateEntities(std::string_view recordJson,
                                       std::string_view schemaJson,
                                       std::span<const std::string_view> selected,
                                       const foundation::identity::EntropySource& entropy,
                                       Bytes& outRecordJson,
                                       IdentityRecordError& outError) noexcept
{
    outError = {};
    if (recordJson.size() > MAX_WORLD_DOCUMENT_BYTES)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World record is too large");
    }
    if (selected.empty() == true)
    {
        return Fail(outError, IdentityRecordStatus::Unsupported, "no entities were selected to duplicate");
    }

    foundation::Array<EntityReferenceProperty> references;
    const IdentityRecordStatus schemaStatus = ReadEntityReferenceProperties(schemaJson, references, outError);
    if (schemaStatus != IdentityRecordStatus::Success)
    {
        return schemaStatus;
    }

    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    if (document.Read(recordJson, parseError, internal::MakeLimits(MAX_WORLD_DOCUMENT_BYTES)) !=
        foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World record is not valid JSON");
    }
    const JsonValue root = document.Root();
    uint64 version = 0;
    if (root.Object() == false || root.Get("version").Integer(version) == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World record has no version");
    }
    if (version != WORLD_RECORD_VERSION_2)
    {
        return Fail(outError, IdentityRecordStatus::UnsupportedVersion, "only version-2 World records are duplicated");
    }
    const JsonValue entities = root.Get("entities");
    if (entities.Array() == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World record has no entities array");
    }
    if (entities.Count() + selected.size() > MAX_DOCUMENT_OBJECTS)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "duplication would exceed the entity limit");
    }

    // Collect every existing identity as the target index for minting.
    foundation::Array<Uuid> taken;
    if (taken.TryEnsureCapacity(entities.Count() + selected.size()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the identity index");
    }
    for (usize i = 0; i < entities.Count(); ++i)
    {
        Uuid existing;
        if (internal::TryReadIdentity(entities.At(i).Get("uuid"), existing) == false)
        {
            return Fail(outError, IdentityRecordStatus::InvalidIdentity, "entity has no nonnil version-4 uuid");
        }
        [[maybe_unused]] const bool isAdded = taken.TryAdd(existing);
        LUDUS_ASSERT(isAdded == true);
    }

    // Resolve each selected UUID to its entity node and mint its clone's new UUID.
    foundation::Array<DuplicationClone> clones;
    if (clones.TryEnsureCapacity(selected.size()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the clone table");
    }
    for (const std::string_view selectedText : selected)
    {
        Uuid oldUuid;
        if (foundation::identity::TryParseUuid(selectedText.data(),
                                               selectedText.size(),
                                               foundation::identity::UuidPolicy::RequireVersion4,
                                               oldUuid) != foundation::identity::UuidStatus::Success)
        {
            return Fail(outError, IdentityRecordStatus::InvalidIdentity, "selected id is not a v4 uuid", selectedText);
        }
        // Reject a selection that is not an entity of this record, or a repeat.
        for (const DuplicationClone& clone : clones)
        {
            if (clone.OldUuid == oldUuid)
            {
                return Fail(outError, IdentityRecordStatus::InvalidIdentity, "selected id repeated", selectedText);
            }
        }
        JsonValue match;
        for (usize i = 0; i < entities.Count(); ++i)
        {
            Uuid entityUuid;
            if (internal::TryReadIdentity(entities.At(i).Get("uuid"), entityUuid) == true && entityUuid == oldUuid)
            {
                match = entities.At(i);
                break;
            }
        }
        if (match.Valid() == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::UnresolvedReference,
                        "selected id names no entity of this record",
                        selectedText);
        }
        Uuid newUuid;
        const IdentityRecordStatus minted = MintUnusedUuid(entropy, taken, newUuid, outError);
        if (minted != IdentityRecordStatus::Success)
        {
            return minted;
        }
        [[maybe_unused]] const bool isTakenAdded = taken.TryAdd(newUuid);
        LUDUS_ASSERT(isTakenAdded == true);
        [[maybe_unused]] const bool isCloneAdded =
            clones.TryAdd(DuplicationClone{ .Entity = match, .OldUuid = oldUuid, .NewUuid = newUuid });
        LUDUS_ASSERT(isCloneAdded == true);
    }

    usize capacity = recordJson.size() * INITIAL_OUTPUT_EXPANSION + OUTPUT_HEADROOM_BYTES;
    while (true)
    {
        Bytes candidate;
        IdentityRecordError attemptError;
        const IdentityRecordStatus status =
            EmitDuplicated(root,
                           std::span<const DuplicationClone>(clones.GetData(), clones.GetSize()),
                           references,
                           capacity,
                           candidate,
                           attemptError);
        if (status == IdentityRecordStatus::Success)
        {
            outRecordJson = Move(candidate);
            return status;
        }
        if (status != IdentityRecordStatus::Limit || capacity >= MAX_OUTPUT_BYTES)
        {
            outError = attemptError;
            return status;
        }
        capacity = capacity * 2 < MAX_OUTPUT_BYTES ? capacity * 2 : MAX_OUTPUT_BYTES;
    }
}
} // namespace ludus::content::identity_records
