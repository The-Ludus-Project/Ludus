#pragma once

// Stage 4 of the object-handles/UUID architecture: two-pass restore at the
// World boundary. See docs/architecture/object-handles-uuid.md, sections
// "Resolution and streaming" ("Required local links are resolved in a second
// pass after all candidate entities exist, preserving forward references and
// the current staged-world transaction"; "Restore uses two passes: create
// objects, then fix up links") and "Persistence and networking" ("Serialize
// neither runtime owner/slot/generation nor object addresses"). Review:
// docs/architecture/object-handles-uuid-reference-review.md.
//
// This in-tree bridge library deliberately depends on both Ludus::ContentIdentity
// (version-2 World record parsing) and Ludus::GameplayWorld (entity creation and
// the durable binding table). It keeps content free of a gameplay dependency and
// keeps gameplay/world free of the authoring-record parser: neither of those
// modules links the other.

#include <ludus/content/content.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/gameplay/world/binding_table.h>
#include <ludus/gameplay/world/entity_registry.h>

#include <span>
#include <string_view>

namespace ludus::content::world_restore
{
/// Result of a two-pass World restore.
enum class RestoreStatus : foundation::uint8
{
    /// Every candidate was created and every required local link resolved; the
    /// bindings were published and the active world updated.
    Success,
    /// The record text is not valid JSON, has the wrong shape, or exceeds a limit.
    Malformed,
    /// The record is not a supported version-2 World record.
    UnsupportedVersion,
    /// The schema JSON is malformed or does not describe the entity-typed properties.
    InvalidSchema,
    /// A UUID field is not a nonnil version-4 identity, or two entities share one.
    InvalidIdentity,
    /// A required local link names a SourceObjectId that is not an entity of this
    /// record. The restore is rejected and the active world is unchanged.
    UnresolvedReference,
    /// The registry or binding table could not create/reserve capacity for the
    /// candidate. The active world is unchanged.
    CapacityExceeded,
    /// An allocation failed while staging the candidate. The active world is unchanged.
    OutOfMemory,
};

/// Return a stable, human-readable name for `status`. Never null.
[[nodiscard]] const char* GetRestoreStatusName(RestoreStatus status) noexcept;

/// @brief Diagnostic detail for a failed restore.
struct RestoreError final
{
    /// The failing status (never RestoreStatus::Success for a reported error).
    RestoreStatus Status = RestoreStatus::Success;
    /// A short human-readable explanation naming the offending input where possible.
    content::MessageText Message;
};

/// @brief Outcome counts for a successful restore.
struct RestoreSummary final
{
    /// Number of entities created in the registry.
    foundation::usize EntityCount = 0;
    /// Number of durable bindings published to the binding table.
    foundation::usize BindingCount = 0;
    /// Number of required local links resolved in the second pass.
    foundation::usize ResolvedLinkCount = 0;
};

/// @brief Restore a version-2 World record into a registry and binding table in two passes.
///
/// The restore is transactional at the World boundary:
///
/// - **Pass 1** creates one entity per record entity in `inoutRegistry` and records
///   its `SourceObjectId -> EntityId` mapping, without resolving any link, so a
///   reference whose target appears later in the record still resolves.
/// - **Pass 2** resolves each required local link (an entity-typed component
///   property naming another entity's `SourceObjectId`) to the entity created in
///   pass 1, then publishes all durable bindings to `inoutBindings` with one
///   atomic `TryActivate`.
///
/// If any step fails — malformed input, an unresolved required link, a duplicate
/// identity, or a capacity/allocation failure — the entities created in pass 1 are
/// released, no binding is published, and the active world is left unchanged.
/// Runtime owner, slot and generation values and object addresses are never read
/// from or written to the record; only durable `SourceObjectId`/`InstanceId`
/// values cross the boundary.
///
/// @param recordJson The version-2 World record text (UTF-8 JSON).
/// @param schemaJson The World schema identifying the entity-typed properties.
/// @param instance The instance scope the activated level binds under; its
/// `InstanceId` comes from the level's placement or save, not minted here. Must be nonnil.
/// @param inoutRegistry A registry with enough spare capacity for every record
/// entity; entities are created here and released again on failure.
/// @param inoutBindings The binding table sized for `inoutRegistry`; durable
/// bindings are published here only on success.
/// @param outSummary Receives the created/published/resolved counts on success; unchanged otherwise.
/// @param outError Receives the failing status and message on failure; set to a
/// cleared value on success.
/// @return RestoreStatus::Success, or a failure status leaving the world unchanged.
[[nodiscard]] RestoreStatus TryRestoreWorld(std::string_view recordJson,
                                            std::string_view schemaJson,
                                            const foundation::identity::InstanceId& instance,
                                            gameplay::world::EntityRegistry& inoutRegistry,
                                            gameplay::world::WorldBindingTable& inoutBindings,
                                            RestoreSummary& outSummary,
                                            RestoreError& outError) noexcept;
} // namespace ludus::content::world_restore
