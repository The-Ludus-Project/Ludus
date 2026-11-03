# World demo harvest and regrow design

Status: proposed, 2026-11-03. Implements the event rules owned by
[randomness.md](../../../docs/architecture/randomness.md#milestone-1-procedural-placement)
in `apps/world_demo`. Read [requirements.md](requirements.md) and
[tasks.md](tasks.md).

## Ownership

`randomness.md` owns the domains, root seed, event rules and isolation; this
spec only consumes them. `procedural-content-generation.md` owns the recipe,
placement identity, thinning rule and kernel; this change does not touch the
kernel. All new code lives in `apps/world_demo` (`world.cpp`, `internal/world.h`,
`internal/level.h`, `level.cpp`, `world_level.cpp`) and its tests. The reserved
`RANDOM_DOMAIN_SCATTER_REGROW = 4` in `internal/random_domains.h` becomes a live
consumer; the manifest is otherwise unchanged.

## Authoring: the harvestable flag (HR01)

`ScatterLayerRecipe` (`internal/level.h`) gains:

```cpp
/// Whether the player can harvest this layer's placements (at most one layer per room). Default false.
bool IsHarvestable = false;
```

Readers set it optionally so committed rooms stay valid:

- `world_level.cpp` `ScatterLayer`: the `harvestable` key is optional. Branch the
  strict `Fields` whitelist on its presence (mirroring `Actor`'s enemy/player
  branch), then read it with `JsonValue::Boolean` when present.
- `level.cpp` (v1 reader used by `ExampleLevel`) accepts an optional
  `harvestable` boolean field on `scatter_layer`.

Validation (`ValidateLevel` in `level.cpp`) rejects a room that marks more than
one layer harvestable, returning `LevelError::InvalidValue` with nothing kept.
`GetReplayHash` folds `IsHarvestable` into the per-layer scatter recipe section,
so the flag is content. Default `false` keeps the first-room digest and replay
hash of existing rooms unchanged.

## Per-placement mutable state (HR02-HR05)

A parallel bounded array, one entry per accepted placement, keyed positionally
to `mPlacements` (whose entry stores the packed scope — the persistent identity):

```cpp
/// Harvest and regrow state of one placement, keyed by its placement's packed scatter scope.
struct PlacementState final
{
    /// Current variant index drawn for the placement: the base variant until a regrow commits a new one.
    uint8 Variant = 0;
    /// Regrow ordinal, starting at 1; the next committed regrow draws (regrow domain, scope, RegrowOrdinal, 0).
    uint32 RegrowOrdinal = 1;
    /// Whether a tombstone suppresses the placement (harvested and not yet regrown).
    bool Harvested = false;
    /// Tick at which a harvested placement becomes eligible to regrow; valid only while Harvested.
    uint64 RegrowAtTick = 0;
    [[nodiscard]] constexpr bool operator==(const PlacementState&) const noexcept = default;
};
```

`GameWorld` adds `foundation::Array<PlacementState> mPlacementStates;` reserved
and filled in `prepareScatter` alongside `mPlacements`, so the only allocation
is a Prepare-time one. `Placement::Variant` stays the base generation variant
(content); the *current* variant for drawing and hashing is `PlacementState`.

### Ordinal semantics (HR04)

- `RegrowOrdinal` starts at 1. The next committed regrow draws the variant at
  ordinal `RegrowOrdinal`, then advances `RegrowOrdinal` by one in `Commit`.
- Thus the first regrow uses ordinal 1 and leaves `RegrowOrdinal == 2`; the
  second uses 2, and so on. This matches "starts at 1, advances exactly once per
  committed regrow".
- Before advancing, the code checks `RegrowOrdinal == UINT32_MAX`. If so the
  regrow fails with an explicit status; the placement stays harvested, the
  ordinal stays `UINT32_MAX`, and no new variant is published. The addressed
  sampler never sees a wrapped ordinal.

## Tick-path mechanics (HR02, HR04)

Harvest and regrow are decided in `Gameplay` (read/stage) and applied in
`Commit` (mutate), matching the existing spawn/destroy command pattern and the
"advances once in the Commit phase" rule.

### Harvest (in `Gameplay`)

When a harvestable layer exists and the player is active, for each placement of
that layer that is not harvested, test overlap of the player's authoritative
box against the placement's drawn box (lattice position / `UnitsPerMetre`,
variant half-extent scaled by the placement scale). On overlap, record a pending
harvest for that placement index (not applied yet).

Harvest detection reuses the existing `Overlap` free function and the
authoritative `transform.Current` of the player — never interpolation, camera or
cosmetic state.

### Scheduling regrow

When a harvest is committed, set `Harvested = true` and
`RegrowAtTick = mTick + REGROW_DELAY_TICKS` (a named constant). In `Gameplay`,
each harvested placement whose `RegrowAtTick <= mTick + 1` (the tick being
committed) is a pending regrow.

### Commit (apply, HR04)

In `Commit`, after the existing spawn/destroy loop, apply the pending harvests
and regrows recorded during `Gameplay`, in ascending placement index for a
deterministic order:

- Pending harvest: `state.Harvested = true`, set `RegrowAtTick`. No RNG.
- Pending regrow: if `state.RegrowOrdinal == UINT32_MAX`, record the explicit
  failure (`mRegrowExhaustions++`) and leave the placement harvested with its
  ordinal and variant unchanged. Otherwise draw
  `TrySampleBounded(mRegrowKey, {scope, RegrowOrdinal, REGROW_VARIANT_DIMENSION},
  variantBound, newVariant)`. On sampler success set `state.Variant = newVariant`,
  `state.Harvested = false`, and advance `state.RegrowOrdinal`. On sampler
  failure (`OutOfRange` after 65536 attempts) leave everything unchanged — the
  placement stays harvested.

Because the mutation happens only in `Commit`, a `Gameplay` that returns a
failure status (so `Commit` never runs) advances nothing; a faulted tick leaves
every ordinal and tombstone intact. A rebuilt or re-prepared world reloads the
session tombstones and resets ordinals from them (see persistence below), and
`BuildNext` never touches placement state.

The pending-harvest and pending-regrow lists are fixed arrays sized
`MAX_ROOM_PLACEMENTS`, reset each tick in `BeginTick`; no tick-path allocation.

A test seam (`internal::WorldTestAccess`, already a friend of `GameWorld`) can
force a placement's `RegrowOrdinal` to `UINT32_MAX` and request/commit a regrow,
to exercise exhaustion without running 2^32 regrows.

## Tombstones survive a room round-trip (HR03)

Tombstones live with the `Session`, not the candidate `GameWorld`, so a failed
candidate never mutates them:

```cpp
/// One session-persistent harvest record, keyed by a placement's packed scatter scope.
struct Tombstone final
{
    uint64 Scope = 0;
    uint32 RegrowOrdinal = 1;
    uint8 Variant = 0;
    bool Harvested = false;
};
```

`Session` holds a bounded `foundation::Array<Tombstone>` per room ordinal (a
small fixed catalog is enough: two rooms). On `Activate`, the session writes back
the candidate world's final placement states for its room ordinal; on
`Prepare`, the candidate reads the session's tombstones for its room and applies
them to `mPlacementStates` after base generation (matching scope to scope). A
placement whose scope has no tombstone keeps the base state (`Harvested=false`,
`RegrowOrdinal=1`, base variant).

To keep `GameWorld::Prepare` free of a `Session` dependency, `Prepare` takes an
optional borrowed view of tombstones (`BorrowedView<const Tombstone>`, empty by
default) that it applies after generation. The session supplies its stored view;
direct `GameWorld` tests pass none. Applying tombstones allocates nothing beyond
the Prepare-time `mPlacementStates` reservation.

## Rendering (HR02, HR05)

`Extract` draws the *current* variant: for each placement it reads
`mPlacementStates[i]`. A harvested (not regrown) placement is skipped (not
drawn). A regrown placement draws `state.Variant` instead of `placement.Variant`.
The cosmetic tint still samples only the cosmetic domain at event zero and
enters nothing. The render-frame draw budget is unchanged (a harvested placement
draws nothing; a regrown one still draws one placement), so `MAX_RENDER_DRAWS`
is unchanged.

## Replay hash (HR05)

In `GetReplayHash`, the per-placement loop additionally folds, per placement:
`state.Harvested`, `state.RegrowOrdinal`, `state.Variant` and `state.RegrowAtTick`.
It also folds `mRegrowExhaustions`. The harvestable flag enters through the
scatter-recipe section. Cosmetic sampling is untouched. Because harvest and
regrow mutate these values, the hash changes with harvest and with a committed
regrow, and is independent of rendering, pacing, interpolation and cosmetic
draws.

## Random domain wiring

`prepareScatter` derives the regrow key once, alongside the cosmetic key:

```cpp
foundation::math::TryMakeRandomKey(worldSeed.Value, RANDOM_DOMAIN_SCATTER_REGROW, regrowKey);
```

stored as `mRegrowKey`. The variant bound is the harvestable layer's variant
count, prepared once with `TryPrepareBound32` in Prepare so the tick path never
divides on setup. A regrow draw uses `BASE_GENERATION_EVENT`-independent
addressing: the event field carries the ordinal, the dimension is
`REGROW_VARIANT_DIMENSION = 0`, exactly as `randomness.md` specifies
`(regrow domain, placement scope, ordinal, 0)`.

## Named constants (no magic numbers)

- `REGROW_VARIANT_DIMENSION = 0` — regrow-domain dimension of the variant draw.
- `REGROW_DELAY_TICKS` — ticks after a committed harvest before a placement is
  eligible to regrow (a named gameplay constant, e.g. a small multiple of
  `TICKS_PER_SECOND`).
- `MAX_HARVESTABLE_LAYERS = 1` — most harvestable layers one room may author.

## Determinism, portability and allocation (HR03, HR05, HR06)

- All arithmetic is integer/exact for identity and ordinal; the variant draw is
  `TrySampleBounded`, which is bit-identical native and web.
- Every allocation (placement store, placement-state store, scratch) is in the
  Prepare path. The pending-harvest/regrow arrays are fixed members. The
  allocation tests' `CANDIDATE_ALLOCATIONS` count grows by one (the placement
  state store); the test updates that constant and still proves the active world
  survives each failure.
- `Commit` mutating placement state performs no allocation.

## Alternatives considered

- *Harvest in `Extract`*: rejected — rendering must not mutate simulation
  (HR02); `Extract` is const.
- *Store current variant on `Placement`*: rejected — `Placement` is the base
  generation record folded into the golden digest; mutating it would move the
  pinned digest. Current variant belongs to the separate `PlacementState`.
- *Tombstones on the candidate world*: rejected — a failed candidate would
  mutate durable state. Tombstones live on the `Session` and are reapplied by
  `Prepare` (HR03).
