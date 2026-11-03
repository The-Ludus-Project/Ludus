# World restore (two-pass)

`Ludus::WorldRestore` (`modules/content/world_restore`, header
`ludus/content/world_restore/world_restore.h`) rebuilds a version-2 World record
into a runtime [`EntityRegistry`](../gameplay/world.md) and its
[durable binding table](../gameplay/world.md#durable-binding-table). It is stage 4
of the [object handles and persistent UUID architecture](../../architecture/object-handles-uuid.md#resolution-and-streaming).

It is an in-tree bridge library: it depends on both `Ludus::ContentIdentity`
(to parse the version-2 record and its schema) and `Ludus::GameplayWorld` (to
create entities and publish bindings), so neither of those modules has to depend
on the other. Tools and tests link it; it is not installed until an SDK consumer
needs it.

## Two passes

`TryRestoreWorld(recordJson, schemaJson, instance, inoutRegistry, inoutBindings,
outSummary, outError)` restores transactionally at the World boundary:

- **Pass 1** creates one entity per record entity in the registry and records its
  `SourceObjectId -> EntityId` mapping, resolving no links. Duplicate entity
  UUIDs and non-version-4 UUIDs are rejected here.
- **Pass 2** resolves every required local link — an entity-typed component
  property that names another entity's `SourceObjectId` — against the entities
  created in pass 1, so a reference whose target appears **later** in the record
  still resolves (forward references). A link that names no entity of the record
  is reported as `UnresolvedReference`.

The durable keys are then published to the binding table with one atomic
`TryActivate`. The caller supplies the activating `InstanceId` — a standalone
level takes it from its placement or save scope, not minted per restore — and a
registry with spare capacity for every record entity.

## Transaction and serialization contract

If any step fails (malformed input, unsupported version, invalid schema, a
duplicate identity, an unresolved required link, or a capacity/allocation
failure), the entities created in pass 1 are released, no binding is published,
and the active world is left exactly as it was. The schema identifies the
entity-typed properties; an empty reference string is an explicit "no target" and
is not a link.

Restore reads and writes only durable identity: `SourceObjectId` and `InstanceId`
values. Runtime owner, slot and generation values and object addresses are never
read from or written to the record, so a restored world always receives fresh
runtime handles while its durable keys stay stable.

`RestoreStatus` values are `Success`, `Malformed`, `UnsupportedVersion`,
`InvalidSchema`, `InvalidIdentity`, `UnresolvedReference`, `CapacityExceeded` and
`OutOfMemory`; `RestoreError` carries the status and a bounded message, and
`RestoreSummary` reports the created/published/resolved counts on success.

See [object handles and persistent UUIDs](../../architecture/object-handles-uuid.md#resolution-and-streaming)
for the full contract and [gameplay world storage](../gameplay/world.md) for the
registry and binding table this restore drives.
