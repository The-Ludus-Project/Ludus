# Gameplay world storage

`Ludus::GameplayWorld` supplies identity and typed sparse pools for a game-owned
world. It depends on FoundationBase and FoundationContainers and is exported in
the installed SDK. Gameplay, input, serialization, scheduling and rendering remain
in the consuming game.

Initialize an `EntityRegistry` with a fixed entity capacity, then initialize each
`ComponentPool<T>` against it during construction. Both operations return explicit
allocation/configuration errors. Pool components must be trivially copyable and
trivially destructible plain state. Initialization reserves dense values, dense
owners and the sparse slot mapping for the whole registry limit. Insertion and
removal subsequently allocate nothing.

`TryCreate` changes its output only on success. `EntityId` contains a nonzero world
identity, slot and generation. `IsAlive` validates all three; `IsActive` also excludes
pending destruction. Release advances the generation and retires an exhausted
slot. World identities and creation sequences fail on exhaustion. Moves transfer
registry ownership and leave the source registry without a world identity.

The game's construction/commit facade owns mutable registry and pool access.
Systems receive a const registry, component values and command recording APIs.
`SetStructuralPhase(false)` prevents registry creation/release and pool membership
changes. These checks remain enabled in release builds. Remove an entity from
**every pool before releasing its registry slot**. Registry release cannot discover
a game's component roster.

`Find` validates pool ownership and the complete dense owner. Its pointer and the
matching owner/value spans are borrowed until structural mutation, pool move or
world replacement. Always check `IsActive` before updating an entity. Pending
entities deliberately retain their values until commit. Persist handles across
phases and asynchronous work. Dense order changes after swap removal; use creation
sequence or authored identity when gameplay priority depends on order.

## Durable binding table

`WorldBindingTable` (header `ludus/gameplay/world/binding_table.h`) is the
World-boundary resolution layer for durable identity. It holds two derived
mappings, published and removed together with object activation and destruction:
a sorted contiguous `PersistentObjectKey -> EntityId` forward map, and a
slot-indexed reverse mapping validated by the handle's owner and generation so a
recycled slot never inherits a former object's UUID. The authored records and the
registry stay authoritative; the table is rebuilt, never a second source of truth.

Size the table once with `TryInitialize(registry.GetCapacity())`. Form each
`PersistentObjectKey` from the activating instance scope's `InstanceId` and an
authored `SourceObjectId`, then publish a batch with `TryActivate`: it validates
every candidate against the registry and the existing bindings, reserves forward
capacity through the allocation-failure seam, and publishes both mappings while
advancing the binding revision once. A failed batch publishes nothing and leaves
the active world unchanged. `DetachBinding` removes an entity's binding inside the
registry destruction order, before the slot is released.

`TryResolve(registry, key, outEntity)` performs no I/O and no implicit loading. It
returns a `ResolveStatus` (`Resolved`, `NotResident`, `Missing`, `TypeMismatch`,
`InvalidReference` or `Conflict`), validates the stored incarnation (owner and
generation) on a hit, and drops a stale binding reporting `Missing` when the
incarnation is gone — so unloading and recreating the same durable key yields a
fresh handle while the key stays resolvable. A cached negative result is
invalidated by comparing `GetBindingRevision()`, a monotonic counter advanced on
every publish or removal.

See [the architecture](../../architecture/game-world.md),
[object handles and persistent UUIDs](../../architecture/object-handles-uuid.md#resolution-and-streaming)
and [the reference game](../../examples/world-demo.md) for composition,
transactional candidate loading and phased tick examples.
