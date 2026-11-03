# World demo harvest and regrow tasks

Implements [requirements.md](requirements.md) per [design.md](design.md). Each
task keeps the build warning-clean, the scatter golden digest unchanged, and the
naming/magic-number/explicit-type rules in `AGENTS.md`.

- [ ] 1. Author the harvestable flag (HR01)
  - [ ] 1.1 Add `bool IsHarvestable = false;` to `ScatterLayerRecipe`
    (`apps/world_demo/internal/level.h`) with a Doxygen comment.
  - [ ] 1.2 Read an optional `harvestable` boolean in the v2 reader
    (`world_level.cpp` `ScatterLayer`), branching the strict `Fields` whitelist
    on its presence.
  - [ ] 1.3 Read an optional `harvestable` boolean in the v1 reader
    (`level.cpp` `scatter_layer`).
  - [ ] 1.4 Reject more than one harvestable layer per room in `ValidateLevel`
    (`LevelError::InvalidValue`, nothing kept). Add `MAX_HARVESTABLE_LAYERS = 1`.
  - [ ] 1.5 Fold `IsHarvestable` into the scatter-recipe section of
    `GetReplayHash` so the flag is content.

- [ ] 2. Per-placement state and domain wiring (HR02, HR04, HR05)
  - [ ] 2.1 Add `struct PlacementState` and
    `foundation::Array<PlacementState> mPlacementStates;` to
    `internal/world.h`, plus `mRegrowKey`, a prepared variant bound, and
    `uint64 mRegrowExhaustions`.
  - [ ] 2.2 In `prepareScatter`, derive the regrow key from
    `RANDOM_DOMAIN_SCATTER_REGROW`, prepare the harvestable layer's variant bound,
    reserve and fill `mPlacementStates` (base variant, ordinal 1, not harvested).
  - [ ] 2.3 Add named constants `REGROW_VARIANT_DIMENSION`, `REGROW_DELAY_TICKS`.

- [ ] 3. Tick-path harvest and regrow (HR02, HR04)
  - [ ] 3.1 Reset the fixed pending-harvest and pending-regrow arrays in
    `BeginTick`.
  - [ ] 3.2 In `Gameplay`, detect player-vs-placement overlap on the harvestable
    layer (authoritative position only) and record pending harvests; schedule and
    record pending regrows for harvested placements due this tick.
  - [ ] 3.3 In `Commit`, apply pending harvests (no RNG) and pending regrows in
    ascending index: exhaustion check against `UINT32_MAX`, `TrySampleBounded` at
    `(regrow domain, scope, ordinal, 0)`, then advance the ordinal only on
    success. Leave outputs unchanged on exhaustion or sampler failure.
  - [ ] 3.4 Confirm a `Gameplay` failure and a faulted tick never reach the
    mutation (Commit is skipped), so ordinals and tombstones do not advance.

- [ ] 4. Tombstone persistence across a room round-trip (HR03)
  - [ ] 4.1 Add `struct Tombstone` and session-side bounded per-room storage.
  - [ ] 4.2 Give `GameWorld::Prepare` an optional
    `BorrowedView<const Tombstone>` applied after generation by matching scope.
  - [ ] 4.3 On `Session::Activate`, write back the active world's placement
    states as tombstones for its room ordinal; on candidate `Prepare`, pass the
    stored tombstones.

- [ ] 5. Rendering (HR02, HR05)
  - [ ] 5.1 In `Extract`, skip harvested placements and draw the current variant
    (`mPlacementStates[i].Variant`) for regrown ones; keep the cosmetic tint and
    the draw budget unchanged.

- [ ] 6. Replay hash (HR05)
  - [ ] 6.1 Fold `Harvested`, `RegrowOrdinal`, `Variant`, `RegrowAtTick` and
    `mRegrowExhaustions` into `GetReplayHash`.

- [ ] 7. Test seam (HR04)
  - [ ] 7.1 Extend `internal::WorldTestAccess` so a test can set a placement's
    `RegrowOrdinal` (to `UINT32_MAX`) and force a harvest/regrow to exercise
    exhaustion without 2^32 regrows.

- [ ] 8. Tests in `apps/world_demo/tests` (HR07)
  - [ ] 8.1 Add a harvestable layer to a room fixture (edit a committed source in
    the test, like the existing scatter tests).
  - [ ] 8.2 The ordinal advances exactly once per committed regrow.
  - [ ] 8.3 The `BuildNext` and fault paths do not advance the ordinal.
  - [ ] 8.4 Forced exhaustion through the seam fails and leaves outputs unchanged.
  - [ ] 8.5 Tombstones survive leaving and re-entering a room.
  - [ ] 8.6 The replay hash changes with harvest and with regrow, but not with
    rendering, pacing, interpolation or cosmetic changes.
  - [ ] 8.7 Update the allocation tests' `CANDIDATE_ALLOCATIONS` (+1 for the
    placement-state store) and keep the no-allocation tick/commit/extract checks
    green.
  - [ ] 8.8 Keep the pinned first-room placement digest unchanged.

- [ ] 9. Documentation (HR08)
  - [ ] 9.1 `randomness.md`: mark milestone 1 delivered; list deferred work.
  - [ ] 9.2 `procedural-content-generation.md`: mark P0 delivered; note harvest
    and regrow delivered.
  - [ ] 9.3 `docs/examples/world-demo.md`: document harvesting, regrow and the
    `harvestable` flag; list deferred work.
  - [ ] 9.4 Doxygen for every new/changed public declaration.

- [ ] 10. Validate
  - [ ] 10.1 `./scripts/check --format`.
  - [ ] 10.2 `python3 scripts/python/source_policy.py --base <merge base>`.
  - [ ] 10.3 Native build and tests; `./scripts/check <preset> --tidy-changed
    --base origin/develop`; the docs checks — run where the toolchain allows and
    report any that cannot run.
