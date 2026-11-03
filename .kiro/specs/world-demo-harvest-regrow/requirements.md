# World demo harvest and regrow requirements

Status: proposed implementation contract, 2026-11-03. This is pull request 3 of
[randomness milestone 1](../../../docs/architecture/randomness.md#milestone-1-procedural-placement)
and slice P0 of the
[procedural content generation architecture](../../../docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter).
Read [design.md](design.md) and [tasks.md](tasks.md).

## Purpose and scope

The world demo already scatters static placements per room from a recorded
session `WorldSeed` (pull requests 1 and 2). This change lets the player harvest
a placement of one authored layer and lets a harvested placement regrow a new
variant later, with the determinism and replay contracts the randomness
architecture owns.

The event rules are owned by
[randomness.md, "Milestone 1: procedural placement"](../../../docs/architecture/randomness.md#milestone-1-procedural-placement);
this spec implements exactly those rules in `apps/world_demo`. It does not change
the scatter kernel's layout version or golden digest, and it does not add
checkpoint persistence (that is milestone 2).

### Out of scope

- Checkpoint persistence of tombstones and regrow ordinals (milestone 2).
- Any change to the scatter kernel (`Ludus::ProceduralEnvironment`) layout
  version `SCATTER_LAYOUT_VERSION` or its golden placement digest.
- Weighted selection and seed acquisition (later randomness milestones).

## Requirements

### HR01 Harvestable layer authoring

- One `scatter_layer` may be marked harvestable in the level data with an
  optional `harvestable` boolean, default `false`. Existing rooms that do not
  author it are unchanged, so the pinned first-room placement digest does not
  move.
- At most one layer per room may be harvestable. A room that marks two layers
  harvestable is rejected at load with nothing published, like every other
  invalid recipe.
- The flag is content, part of the scatter recipe folded into the replay hash;
  it is not a random draw. The v1 interchange writer still omits scatter, so
  `WriteLevel` is unchanged.

### HR02 Harvest is a world mutation keyed by persistent identity

- A placement of the harvestable layer is harvested when the player overlaps it
  during a tick. Harvesting records a tombstone keyed by the placement's
  persistent identity, which is its packed scatter scope
  (`ProceduralEnvironment::PackScatterScope`).
- A tombstone is a world mutation, not a random event. It never advances any RNG
  event or ordinal.
- Harvest detection reads only durable simulation state (the player's
  authoritative position, the placement's lattice position and the variant's
  drawn extent). It never reads interpolation alpha, a camera, a cosmetic sample
  or any presentation value.
- Harvest is evaluated and committed on the tick path only (never during
  `Extract`), so rendering choices cannot harvest or un-harvest a placement.

### HR03 Tombstones survive leaving and re-entering a room

- Regenerating a room (`Prepare`) reapplies every tombstone the session holds
  for that room, so a harvested placement stays harvested across leaving and
  re-entering the room within a session.
- Tombstone storage is bounded (at most one per placement, `MAX_ROOM_PLACEMENTS`)
  and lives with the session, not the candidate world, so a failed candidate
  never mutates it. Allocation happens in a Prepare-time path, never on the tick
  path.

### HR04 Regrow ordinal advances exactly once per committed regrow

- Each placement has a regrow ordinal that starts at 1. It advances exactly once,
  in the `Commit` phase of the tick that commits the regrow.
- The regrow draws the new variant with `TrySampleBounded` at the address
  `(regrow domain from the manifest, placement scope, ordinal, dimension 0)`,
  below the harvestable layer's variant count.
- Rebuilding a candidate world, retrying `BuildNext`, or faulting a tick must
  neither advance nor publish an ordinal. A retried evaluation reuses the same
  ordinal and produces the same variant.
- An ordinal that would pass `UINT32_MAX` fails the regrow with an explicit
  status and leaves the placement harvested. It never wraps or narrows, and it
  publishes no new variant.

### HR05 Replay hash and storage

- Tombstones and regrow ordinals enter the world replay hash. The current
  harvested/regrown variant of a placement enters it too, because it is
  simulation state. Cosmetic samples stay out of the replay hash.
- Harvesting a placement changes the replay hash. Committing a regrow changes the
  replay hash. Rendering, frame pacing, interpolation and extra cosmetic draws
  never change it.
- All harvest/regrow storage is bounded and allocated in a Prepare-time path.
  The tick, commit and extraction paths allocate nothing.

### HR06 Determinism and isolation

- The same session seed, recipe and tick inputs produce the same placements,
  tombstones, ordinals and variants, and the same replay hash, on native and
  Emscripten builds.
- Adding a domain or dimension, or changing the transform ranges, does not change
  base placements, tombstones or regrow outcomes.
- Forced ordinal exhaustion through a test seam fails the regrow and leaves the
  placement's outputs (harvested state, ordinal, variant) unchanged.

### HR07 Tests in `apps/world_demo/tests`

The milestone is accepted when tests show:

- The ordinal advances exactly once per committed regrow.
- The `BuildNext` and fault paths do not advance the ordinal.
- Forced exhaustion through a test seam fails and leaves outputs unchanged.
- Tombstones survive leaving and re-entering a room.
- The replay hash changes with harvest and regrow, but not with rendering or
  cosmetic changes.
- The allocation tests stay clean (ticks, commits and extraction never allocate;
  every Prepare-time allocation failure preserves the active world).

### HR08 Documentation

- Update the status in [randomness.md](../../../docs/architecture/randomness.md),
  [procedural-content-generation.md](../../../docs/architecture/procedural-content-generation.md)
  and [docs/examples/world-demo.md](../../../docs/examples/world-demo.md) to mark
  milestone 1 / P0 delivered, listing what stays deferred: the checkpoint codec
  (milestone 2), weighted selection and seed acquisition.
- Document every new or changed public declaration with Doxygen `///` comments
  describing the contract, and keep the one-source publication rules.
