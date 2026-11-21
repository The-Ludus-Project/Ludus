// Staged scatter generation contracts (slice P1): two independently keyed
// scopes, precise cache invalidation, explanations of removed placements by
// persistent identity, author locks, stale candidates, cache corruption recovery
// and no publication on failure. See
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include <ludus/content/procedural/environment/scatter_generation.h>

#include "scatter_golden.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::procedural;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

namespace
{
constexpr GenerationCacheLimits CACHE_LIMITS{64, 1U << 20U};
constexpr usize TRACE_CAPACITY = 64;
constexpr uint16 ROOM_A = 1;
constexpr uint16 ROOM_B = 2;
constexpr uint8 TREE_LAYER = 1;
constexpr uint8 STONE_LAYER = 2;

constexpr ScatterVariant TREE_VARIANTS[] = {{10U}, {20U}};
constexpr ScatterVariant STONE_VARIANTS[] = {{5U}};
constexpr ScatterRectangle ROOM_B_KEEP_OUTS[] = {{0, 0, 50, 50}};
/// The keep-out edit of room A, with known removals: every accepted placement inside it.
constexpr ScatterRectangle ROOM_A_EDIT[] = {{100, 80, 220, 200}};

/// Two rooms: A with trees and stones, B with trees and a keep-out. Views borrow the fixture's own arrays.
struct TwoScopeFixture final
{
    TwoScopeFixture()
    {
        RoomALayers[0] = {TREE_LAYER, 40, 3, BorrowedView<const ScatterVariant>(TREE_VARIANTS), 900, 1100};
        RoomALayers[1] = {STONE_LAYER, 25, 2, BorrowedView<const ScatterVariant>(STONE_VARIANTS), 1000, 1000};
        RoomBLayers[0] = {TREE_LAYER, 30, 2, BorrowedView<const ScatterVariant>(TREE_VARIANTS), 800, 1200};
        Scopes[0].Recipe = {ROOM_A, {0, 0, 400, 300}, {}, BorrowedView<const ScatterLayer>(RoomALayers)};
        Scopes[1].Recipe = {ROOM_B,
                            {-200, -100, 200, 200},
                            BorrowedView<const ScatterRectangle>(ROOM_B_KEEP_OUTS),
                            {RoomBLayers}};
        Input.WorldSeed = golden::WORLD_SEED;
        Input.Domains = golden::DOMAINS;
        Input.Scopes = BorrowedView<const ScatterScope>(Scopes);
    }
    TwoScopeFixture(const TwoScopeFixture&) = delete;
    TwoScopeFixture& operator=(const TwoScopeFixture&) = delete;

    ScatterLayer RoomALayers[2] = {};
    ScatterLayer RoomBLayers[1] = {};
    ScatterScope Scopes[2] = {};
    ScatterGenerationInput Input;
};

[[nodiscard]] bool AreEqual(const ScatterPlacement& a, const ScatterPlacement& b) noexcept
{
    return a.Identity == b.Identity && a.LatticeX == b.LatticeX && a.LatticeY == b.LatticeY && a.Variant == b.Variant &&
           a.Rotation == b.Rotation && a.Scale == b.Scale;
}

[[nodiscard]] bool Contains(const ScatterRectangle& rectangle, const ScatterPlacement& placement) noexcept
{
    return placement.LatticeX >= rectangle.MinimumX && placement.LatticeX < rectangle.MaximumX &&
           placement.LatticeY >= rectangle.MinimumY && placement.LatticeY < rectangle.MaximumY;
}

void RequireSameManifest(const ScatterManifest& expected, const ScatterManifest& actual)
{
    REQUIRE(actual.Digest == expected.Digest);
    REQUIRE(actual.Layers.GetSize() == expected.Layers.GetSize());
    REQUIRE(actual.Placements.GetSize() == expected.Placements.GetSize());
    for (usize i = 0; i < expected.Placements.GetSize(); ++i)
    {
        REQUIRE(AreEqual(actual.Placements[i], expected.Placements[i]) == true);
    }
}

[[nodiscard]] const GenerationStageRecord& FindRecord(const GenerationTrace& trace, uint16 room, uint8 layer)
{
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        if (record.Scope == room && record.Stage == layer)
        {
            return record;
        }
    }
    FAIL("no trace record for the stage");
    return trace.GetRecords()[0];
}

/// Stage and publish the fixture's baseline.
void PublishBaseline(ScatterGenerationCoordinator& inoutCoordinator, const ScatterGenerationInput& input)
{
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate candidate;
    REQUIRE(inoutCoordinator.TryStage(input, trace, candidate) == GenerationStatus::Success);
    REQUIRE(inoutCoordinator.TryPublish(candidate) == GenerationStatus::Success);
}

/// An accepted room-A tree outside the keep-out edit, which the author locks.
[[nodiscard]] ScatterPlacement FindProtectedPlacement(const ScatterManifest& accepted)
{
    for (const ScatterPlacement& placement : accepted.Placements)
    {
        if (placement.Identity.Room == ROOM_A && placement.Identity.Layer == TREE_LAYER &&
            Contains(ROOM_A_EDIT[0], placement) == false)
        {
            return placement;
        }
    }
    FAIL("the fixture has no placement outside the keep-out edit");
    return {};
}
} // namespace

TEST_CASE("A staged keep-out edit is explained by placement before publication", "[procedural][generation]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;

    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate baseline;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, baseline) == GenerationStatus::Success);
    REQUIRE(trace.GetRecords().GetSize() == 3);
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        REQUIRE(record.Outcome == GenerationStageOutcome::Executed);
        REQUIRE(record.Cache == GenerationCacheOutcome::Absent);
        REQUIRE(record.IsStored == true);
    }
    REQUIRE(coordinator.GetAcceptedRevision() == 0);
    REQUIRE(coordinator.TryPublish(baseline) == GenerationStatus::Success);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    REQUIRE(baseline.Status == GenerationStatus::NotReady);
    REQUIRE(baseline.Manifest.Placements.IsEmpty() == true);
    const ScatterManifest accepted = coordinator.GetAccepted();
    REQUIRE(accepted.Layers.GetSize() == 3);

    // Known removals: accepted room-A placements of either layer inside the edit.
    Array<ScatterIdentity> knownRemovals;
    usize roomBCount = 0;
    for (const ScatterPlacement& placement : accepted.Placements)
    {
        if (placement.Identity.Room == ROOM_A && Contains(ROOM_A_EDIT[0], placement) == true)
        {
            knownRemovals.Add(placement.Identity);
        }
        roomBCount += placement.Identity.Room == ROOM_B ? 1 : 0;
    }
    REQUIRE(knownRemovals.GetSize() > 0);
    REQUIRE(roomBCount > 0);
    const ScatterPlacement protectedPlacement = FindProtectedPlacement(accepted);

    fixture.Scopes[0].Recipe.KeepOuts = BorrowedView<const ScatterRectangle>(ROOM_A_EDIT);
    fixture.Scopes[0].Locks = {&protectedPlacement, 1};
    trace.Clear();
    ScatterCandidate edited;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, edited) == GenerationStatus::Success);
    REQUIRE(edited.BaseRevision == 1);
    REQUIRE(edited.Conflicts.IsEmpty() == true);
    // The unchanged scope reuses its cached product; the edited scope's layers run again.
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(FindRecord(trace, ROOM_A, TREE_LAYER).Outcome == GenerationStageOutcome::Executed);
    REQUIRE(FindRecord(trace, ROOM_A, STONE_LAYER).Outcome == GenerationStageOutcome::Executed);
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Key == accepted.Layers[2].Key);

    Array<ScatterChange> changes;
    REQUIRE(TryCompareScatterManifests(accepted, edited.Manifest, fixture.Input, changes) == GenerationStatus::Success);
    usize removedCount = 0;
    for (const ScatterChange& change : changes)
    {
        REQUIRE(change.Identity.Room == ROOM_A);
        REQUIRE(change.Kind != ScatterChangeKind::Changed);
        if (change.Kind == ScatterChangeKind::Added)
        {
            // Removing a conflicting neighbour can admit a candidate that thinning rejected before.
            REQUIRE(accepted.FindPlacement(change.Identity) == nullptr);
            REQUIRE(edited.Manifest.FindPlacement(change.Identity) != nullptr);
            continue;
        }
        ++removedCount;
        bool isKnown = false;
        for (const ScatterIdentity& identity : knownRemovals)
        {
            isKnown = isKnown == true || identity == change.Identity;
        }
        REQUIRE(isKnown == true);
        REQUIRE(change.Decision.Stage == ScatterDecisionStage::Eligibility);
        REQUIRE(change.Decision.Rule == ScatterDecisionRule::KeepOut);
        REQUIRE(change.Decision.KeepOut == 0);
    }
    REQUIRE(removedCount == knownRemovals.GetSize());

    // The protected placement and every placement of the unaffected scope keep their identities and values.
    const ScatterPlacement* protectedOrNull = edited.Manifest.FindPlacement(protectedPlacement.Identity);
    REQUIRE(protectedOrNull != nullptr);
    REQUIRE(AreEqual(*protectedOrNull, protectedPlacement) == true);
    for (const ScatterPlacement& placement : accepted.Placements)
    {
        if (placement.Identity.Room == ROOM_B)
        {
            const ScatterPlacement* generatedOrNull = edited.Manifest.FindPlacement(placement.Identity);
            REQUIRE(generatedOrNull != nullptr);
            REQUIRE(AreEqual(*generatedOrNull, placement) == true);
        }
    }

    // A clean cook of the same inputs, with an empty cache, produces the same manifest.
    MemoryGenerationCacheStore cleanStore(CACHE_LIMITS);
    ScatterGenerationCoordinator cleanCoordinator(cleanStore);
    GenerationTrace cleanTrace(TRACE_CAPACITY);
    ScatterCandidate clean;
    REQUIRE(cleanCoordinator.TryStage(fixture.Input, cleanTrace, clean) == GenerationStatus::Success);
    RequireSameManifest(clean.Manifest, edited.Manifest);

    // Rejecting the candidate preserves the accepted placements and manifest.
    coordinator.Reject(edited);
    REQUIRE(edited.Status == GenerationStatus::NotReady);
    REQUIRE(coordinator.TryPublish(edited) == GenerationStatus::NotReady);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    RequireSameManifest(accepted, coordinator.GetAccepted());
}

TEST_CASE("Each layer product is invalidated only by its own inputs", "[procedural][generation][cache]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    PublishBaseline(coordinator, fixture.Input);
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate candidate;

    SECTION("a stone scale edit reruns only the stone layer")
    {
        fixture.RoomALayers[1].MaximumScale = 1200;
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::Success);
        REQUIRE(FindRecord(trace, ROOM_A, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
        REQUIRE(FindRecord(trace, ROOM_A, STONE_LAYER).Outcome == GenerationStageOutcome::Executed);
        REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
        REQUIRE(candidate.Manifest.Digest != coordinator.GetAccepted().Digest);
    }
    SECTION("adding a lock changes no product")
    {
        const ScatterPlacement lock = coordinator.GetAccepted().Placements[0];
        fixture.Scopes[0].Locks = {&lock, 1};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::Success);
        for (const GenerationStageRecord& record : trace.GetRecords())
        {
            REQUIRE(record.Outcome == GenerationStageOutcome::CacheHit);
        }
        RequireSameManifest(coordinator.GetAccepted(), candidate.Manifest);
    }
    SECTION("a seed edit reruns every scope")
    {
        fixture.Input.WorldSeed += 1;
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::Success);
        for (const GenerationStageRecord& record : trace.GetRecords())
        {
            REQUIRE(record.Outcome == GenerationStageOutcome::Executed);
        }
    }
}

TEST_CASE("An unsatisfied author lock blocks publication and names its cause", "[procedural][generation][lock]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    PublishBaseline(coordinator, fixture.Input);
    const ScatterManifest accepted = coordinator.GetAccepted();
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate candidate;

    SECTION("a keep-out over the locked placement removes it")
    {
        const ScatterPlacement lock = accepted.Placements[0];
        const ScatterRectangle cover[] = {{lock.LatticeX, lock.LatticeY, lock.LatticeX + 1, lock.LatticeY + 1}};
        fixture.Scopes[0].Recipe.KeepOuts = BorrowedView<const ScatterRectangle>(cover);
        fixture.Scopes[0].Locks = {&lock, 1};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::ConstraintFailed);
        REQUIRE(candidate.Conflicts.GetSize() == 1);
        REQUIRE(candidate.Conflicts[0].IsMissing == true);
        REQUIRE(candidate.Conflicts[0].Lock.Identity == lock.Identity);
        REQUIRE(candidate.Conflicts[0].Decision.Rule == ScatterDecisionRule::KeepOut);
        REQUIRE(candidate.Manifest.FindPlacement(lock.Identity) == nullptr);
    }
    SECTION("a lock whose values the recipe no longer produces")
    {
        ScatterPlacement lock = accepted.Placements[0];
        lock.Rotation = static_cast<uint16>(lock.Rotation + 1U);
        fixture.Scopes[0].Locks = {&lock, 1};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::ConstraintFailed);
        REQUIRE(candidate.Conflicts.GetSize() == 1);
        REQUIRE(candidate.Conflicts[0].IsMissing == false);
        REQUIRE(AreEqual(candidate.Conflicts[0].Generated, accepted.Placements[0]) == true);
    }
    SECTION("a lock whose layer was removed")
    {
        ScatterPlacement lock = accepted.Placements[0];
        lock.Identity.Layer = STONE_LAYER + 1;
        fixture.Scopes[0].Locks = {&lock, 1};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::ConstraintFailed);
        REQUIRE(candidate.Conflicts[0].Decision.Stage == ScatterDecisionStage::Recipe);
        REQUIRE(candidate.Conflicts[0].Decision.Rule == ScatterDecisionRule::LayerRemoved);
    }
    REQUIRE(candidate.Status == GenerationStatus::ConstraintFailed);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    RequireSameManifest(accepted, coordinator.GetAccepted());
}

TEST_CASE("A candidate staged against an older revision cannot publish", "[procedural][generation]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    PublishBaseline(coordinator, fixture.Input);
    GenerationTrace trace(TRACE_CAPACITY);

    ScatterCandidate first;
    ScatterCandidate second;
    fixture.Scopes[0].Recipe.KeepOuts = BorrowedView<const ScatterRectangle>(ROOM_A_EDIT);
    REQUIRE(coordinator.TryStage(fixture.Input, trace, first) == GenerationStatus::Success);
    fixture.Input.WorldSeed += 1;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, second) == GenerationStatus::Success);
    const ScatterManifest firstManifest = first.Manifest;
    REQUIRE(coordinator.TryPublish(first) == GenerationStatus::Success);
    REQUIRE(coordinator.TryPublish(second) == GenerationStatus::StaleCandidate);
    REQUIRE(second.Status == GenerationStatus::Success);
    REQUIRE(second.Manifest.Placements.IsEmpty() == false);
    REQUIRE(coordinator.GetAcceptedRevision() == 2);
    RequireSameManifest(firstManifest, coordinator.GetAccepted());
}

TEST_CASE("A corrupt cache entry is discarded and rebuilt to the same manifest", "[procedural][generation][cache]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    PublishBaseline(coordinator, fixture.Input);
    const ScatterManifest accepted = coordinator.GetAccepted();
    const GenerationDigest roomBKey = accepted.Layers[2].Key;

    SECTION("a flipped byte fails the entry digest")
    {
        BorrowedView<uint8> entry = store.FindEntryBytes(roomBKey);
        REQUIRE(entry.IsEmpty() == false);
        entry[entry.GetSize() - 1] ^= 1U;
    }
    SECTION("a well-framed entry whose content does not fit the layer")
    {
        const uint8 wrongContent[] = {1, 0, 0, 0, 9, 9, 9};
        REQUIRE(TryStoreGenerationProduct(store,
                                          roomBKey,
                                          SCATTER_LAYER_PRODUCT_SCHEMA,
                                          BorrowedView<const uint8>(wrongContent)) == true);
    }
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate rebuilt;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, rebuilt) == GenerationStatus::Success);
    const GenerationStageRecord& record = FindRecord(trace, ROOM_B, TREE_LAYER);
    REQUIRE(record.Cache == GenerationCacheOutcome::Corrupt);
    REQUIRE(record.Outcome == GenerationStageOutcome::Executed);
    REQUIRE(record.IsStored == true);
    REQUIRE(FindRecord(trace, ROOM_A, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
    RequireSameManifest(accepted, rebuilt.Manifest);

    // The rebuilt entry serves the next request.
    trace.Clear();
    REQUIRE(coordinator.TryStage(fixture.Input, trace, rebuilt) == GenerationStatus::Success);
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
}

TEST_CASE("A refused cache store never changes the generated manifest", "[procedural][generation][cache]")
{
    MemoryGenerationCacheStore full(GenerationCacheLimits{0, 0});
    ScatterGenerationCoordinator coordinator(full);
    MemoryGenerationCacheStore roomy(CACHE_LIMITS);
    ScatterGenerationCoordinator reference(roomy);
    TwoScopeFixture fixture;
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate uncached;
    ScatterCandidate cached;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, uncached) == GenerationStatus::Success);
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        REQUIRE(record.IsStored == false);
    }
    REQUIRE(reference.TryStage(fixture.Input, trace, cached) == GenerationStatus::Success);
    RequireSameManifest(cached.Manifest, uncached.Manifest);
}

TEST_CASE("Invalid generation requests stage nothing and publish nothing", "[procedural][generation]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    PublishBaseline(coordinator, fixture.Input);
    const ScatterManifest accepted = coordinator.GetAccepted();
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate candidate;

    SECTION("scopes out of room order")
    {
        const ScatterScope reversed[] = {fixture.Scopes[1], fixture.Scopes[0]};
        fixture.Input.Scopes = BorrowedView<const ScatterScope>(reversed);
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("a lock in another room")
    {
        const ScatterPlacement lock = accepted.Placements[accepted.Placements.GetSize() - 1];
        REQUIRE(lock.Identity.Room == ROOM_B);
        fixture.Scopes[0].Locks = {&lock, 1};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("an invalid recipe")
    {
        fixture.RoomBLayers[0].CellSize = 0;
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("no scopes")
    {
        fixture.Input.Scopes = {};
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::InvalidInput);
    }
    REQUIRE(candidate.Manifest.Placements.IsEmpty() == true);
    REQUIRE(candidate.Manifest.Layers.IsEmpty() == true);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
    RequireSameManifest(accepted, coordinator.GetAccepted());
    Array<ScatterChange> changes;
    REQUIRE(TryCompareScatterManifests(accepted, candidate.Manifest, fixture.Input, changes) ==
            GenerationStatus::InvalidInput);
}
