#pragma once

// Version-2 World records: the version-1 entity/component envelope plus durable
// UUIDs. See docs/architecture/object-handles-uuid.md#stage-3-uuid-authoring-records.
#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/identity/uuid.hpp>

#include <span>
#include <string_view>

namespace ludus::content::identity_records
{
/// Envelope version of a World document whose objects carry only scoped string IDs.
inline constexpr uint64 WORLD_RECORD_VERSION_1 = 1;
/// Envelope version of a World document with a document UUID and one source-object UUID per entity.
inline constexpr uint64 WORLD_RECORD_VERSION_2 = 2;
/// Largest accepted World schema text, in bytes (the Editor's schema limit).
inline constexpr usize MAX_WORLD_SCHEMA_BYTES = usize{64} * 1024;

/// One schema property whose value names another entity of the same World.
struct EntityReferenceProperty final
{
    /// Component ID in the World schema.
    ResourceId Component;
    /// Property ID within that component.
    ResourceId Property;
};

/// @brief Collect the `entity`-typed properties of a World schema.
///
/// Reads the Editor's schema shape (`components[].id` and
/// `components[].properties[]` with `id` and `type`) and ignores the other members,
/// which the Editor validates.
/// @param schemaJson World schema text, at most MAX_WORLD_SCHEMA_BYTES.
/// @param outProperties Replaced on success, in schema order; unchanged on failure.
/// @param outError Receives the failure; reset to Success on success.
/// @return Success, Malformed, Limit or OutOfMemory.
[[nodiscard]] IdentityRecordStatus
ReadEntityReferenceProperties(std::string_view schemaJson,
                              foundation::Array<EntityReferenceProperty>& outProperties,
                              IdentityRecordError& outError) noexcept;

/// @brief Replay a committed manifest to turn one version-1 World document into a version-2 record.
///
/// The output is
/// `{"version": 2, "schema", "document": <AssetId>, "entities": [{"uuid", "label", "name", "components"}]}`.
/// Each entity's version-1 `id` becomes its `label`; `uuid` is the manifest's
/// SourceObjectId for that ID. A nonempty value of a property listed in
/// `references` is rewritten from the target's legacy ID to the target's UUID;
/// an empty value stays empty (no target). Entity order, the display `name` and
/// every other value are preserved; output uses two-space indentation and a
/// trailing newline. Replay is pure: it mints nothing, reads no files, and the
/// same inputs always produce the same bytes.
///
/// The manifest must describe exactly the document's IDs. A World entity missing
/// from the manifest, or a manifest object missing from the World, reports
/// StaleManifest instead of minting or dropping an identity. Documents that use
/// the Editor's `scene` extension, whose parent links are not yet migrated, report
/// Unsupported.
/// @param manifest Committed manifest.
/// @param source The document's path as recorded in the manifest, and its version-1 World JSON
/// (at most MAX_WORLD_DOCUMENT_BYTES).
/// @param references Entity-typed properties, normally from ReadEntityReferenceProperties().
/// @param outWorldJson Replaced on success; unchanged on failure.
/// @param outError Receives the failure; reset to Success on success.
/// @return Success, Malformed, UnsupportedVersion, Unsupported, UnknownDocument, StaleManifest,
/// InvalidLegacyId, UnresolvedReference, Limit or OutOfMemory.
[[nodiscard]] IdentityRecordStatus ApplyWorldMigration(const MigrationManifest& manifest,
                                                       const MigrationSource& source,
                                                       std::span<const EntityReferenceProperty> references,
                                                       Bytes& outWorldJson,
                                                       IdentityRecordError& outError) noexcept;

/// @brief Duplicate a set of entities of a version-2 World record as one transaction.
///
/// Stage 4 graph duplication (docs/architecture/object-handles-uuid.md,
/// "Authoring and prefab instances"). The selected entities are copied as a group:
///
/// 1. A fresh `SourceObjectId` is minted for each selected entity with
///    TryGenerateUuidV4 and `entropy`, checked against every identity already in
///    the record and those minted so far. A colliding candidate is regenerated at
///    most MAX_UUID_GENERATION_ATTEMPTS times before IdentityConflict is reported.
/// 2. The old-to-new table is built, the selected entities are cloned, and inside
///    each clone every entity-typed reference whose target is in the copied set is
///    rewritten to the target's new UUID. References to entities outside the copied
///    set are deliberately retained unchanged.
/// 3. The clones are appended to the record and the whole document is re-emitted.
///
/// The transaction is all-or-nothing: on any failure — a malformed record, an
/// unknown selected UUID, an unavailable entropy source, or an unresolved identity
/// conflict — nothing is minted into the output and `outRecordJson` is unchanged.
/// Only durable `SourceObjectId` values are produced; no runtime handle, owner,
/// slot or generation is read or written. Each clone's `label` is left equal to
/// the source's; callers that need unique labels assign them separately.
///
/// @param recordJson A version-2 World record (at most MAX_WORLD_DOCUMENT_BYTES).
/// @param schemaJson The World schema identifying the entity-typed properties.
/// @param selected The canonical UUID texts of the entities to duplicate; each must
/// be an entity of the record and each must be distinct.
/// @param entropy The injected entropy source used to mint the new UUIDs.
/// @param outRecordJson Replaced with the version-2 record that also contains the
/// clones on success; unchanged on failure.
/// @param outError Receives the failure; reset to Success on success.
/// @return Success, Malformed, UnsupportedVersion, InvalidIdentity, InvalidSchema's
/// Malformed, UnresolvedReference, EntropyUnavailable, IdentityConflict, Limit or OutOfMemory.
[[nodiscard]] IdentityRecordStatus DuplicateEntities(std::string_view recordJson,
                                                     std::string_view schemaJson,
                                                     std::span<const std::string_view> selected,
                                                     const foundation::identity::EntropySource& entropy,
                                                     Bytes& outRecordJson,
                                                     IdentityRecordError& outError) noexcept;
} // namespace ludus::content::identity_records
