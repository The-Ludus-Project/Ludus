// Staged scatter generation contracts (slice P1): two independently keyed
// scopes, precise cache invalidation, explanations of removed placements by
// persistent identity, author locks, stale candidates, cache corruption recovery,
// no publication on failure, and demand-driven requests for one scope's output
// over a shared upstream stage. See
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include "generation_fixture.h"

#include <ludus/content/procedural/environment/scatter_generation.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::procedural;
using namespace ludus::content::procedural::generation_fixture;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

namespace
{
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
    // The comparison after the sections reads fixture.Input again, so the storage a section points
    // it at must outlive the section.
    const ScatterScope reversed[] = {fixture.Scopes[1], fixture.Scopes[0]};
    const ScatterPlacement lock = accepted.Placements[accepted.Placements.GetSize() - 1];

    SECTION("scopes out of room order")
    {
        fixture.Input.Scopes = BorrowedView<const ScatterScope>(reversed);
        REQUIRE(coordinator.TryStage(fixture.Input, trace, candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("a lock in another room")
    {
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

namespace
{
/// Begin, request every scope and publish the fixture's current plan.
void PublishPlan(GenerationCoordinator& inoutCoordinator, const DemandFixture& fixture)
{
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    REQUIRE(inoutCoordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
    REQUIRE(inoutCoordinator.TryRequestAll(fixture.GetPlan(), trace, candidate) == GenerationStatus::Success);
    REQUIRE(inoutCoordinator.TryPublish(candidate) == GenerationStatus::Success);
}

/// The products of one scope, in plan order.
[[nodiscard]] Array<GenerationProduct> GetScopeProducts(BorrowedView<const GenerationProduct> products, uint32 scope)
{
    Array<GenerationProduct> selected;
    for (const GenerationProduct& product : products)
    {
        if (product.Scope == scope)
        {
            selected.Add(product);
        }
    }
    return selected;
}

void RequireSameScope(const GenerationManifest& expected, const GenerationCandidate& actual, uint32 scope)
{
    const Array<GenerationProduct> expectedProducts =
        GetScopeProducts({expected.Products.GetData(), expected.Products.GetSize()}, scope);
    const Array<GenerationProduct> actualProducts =
        GetScopeProducts({actual.Products.GetData(), actual.Products.GetSize()}, scope);
    RequireSameProducts({expectedProducts.GetData(), expectedProducts.GetSize()},
                        {actualProducts.GetData(), actualProducts.GetSize()});
}

/// Request each room in turn after an edit and check the final manifest against a clean full cook.
void RequireCleanCookAfterPublishing(GenerationCoordinator& inoutCoordinator,
                                     const DemandFixture& fixture,
                                     GenerationCandidate& inoutCandidate)
{
    const GenerationManifest clean = CookClean(fixture.GetPlan());
    RequireSameProducts({clean.Products.GetData(), clean.Products.GetSize()},
                        {inoutCandidate.Products.GetData(), inoutCandidate.Products.GetSize()});
    const uint64 revision = inoutCoordinator.GetAcceptedRevision();
    REQUIRE(inoutCoordinator.TryPublish(inoutCandidate) == GenerationStatus::Success);
    REQUIRE(inoutCoordinator.GetAcceptedRevision() == revision + 1);
    REQUIRE(inoutCoordinator.GetAccepted().Digest == clean.Digest);
    RequireSameContents(ReadContents(clean), ReadContents(inoutCoordinator.GetAccepted()));
}
} // namespace

TEST_CASE("Requesting one scope leaves an edited scope pending until it is requested",
          "[procedural][generation][demand]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    GenerationCoordinator coordinator(store);
    DemandFixture fixture;
    fixture.Rebuild();
    PublishPlan(coordinator, fixture);
    const GenerationManifest accepted = coordinator.GetAccepted();
    const ManifestContents acceptedContents = ReadContents(accepted);

    // Edit only B's local input.
    fixture.RoomBSpans[1].Length = 95;
    fixture.Rebuild();
    GenerationCandidate candidate;
    REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
    REQUIRE(candidate.IsScopeReady(ROOM_A) == false);

    // Requesting A evaluates the shared catalog and A's stages, all cache hits, and nothing of B.
    GenerationTrace trace(TRACE_CAPACITY);
    REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_A, trace, candidate) == GenerationStatus::Success);
    REQUIRE(trace.GetRecords().GetSize() == 4);
    REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == 4);
    REQUIRE(FindRecord(trace, SHARED_SCOPE, CATALOG_STAGE).Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(FindRecord(trace, ROOM_A, ASSEMBLY_STAGE).Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(HasScopeRecord(trace, ROOM_B) == false);
    REQUIRE(candidate.IsScopeReady(ROOM_A) == true);
    RequireSameScope(accepted, candidate, ROOM_A);

    // B stays pending, so the candidate cannot publish and the accepted manifest is untouched.
    REQUIRE(candidate.IsScopeReady(ROOM_B) == false);
    REQUIRE(candidate.FindProduct(ROOM_B, TREE_LAYER)->State == GenerationProductState::Pending);
    REQUIRE(candidate.FindProduct(ROOM_B, ASSEMBLY_STAGE)->State == GenerationProductState::Pending);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    REQUIRE(coordinator.GetAccepted().Digest == accepted.Digest);

    // Requesting B runs only its edited assembly; the shared catalog is already ready.
    trace.Clear();
    REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_B, trace, candidate) == GenerationStatus::Success);
    REQUIRE(trace.GetRecords().GetSize() == 2);
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(FindRecord(trace, ROOM_B, ASSEMBLY_STAGE).Outcome == GenerationStageOutcome::Executed);
    REQUIRE(HasScopeRecord(trace, SHARED_SCOPE) == false);
    REQUIRE(candidate.FindProduct(ROOM_B, ASSEMBLY_STAGE)->Key != accepted.FindProduct(ROOM_B, ASSEMBLY_STAGE)->Key);
    REQUIRE(candidate.FindProduct(ROOM_B, TREE_LAYER)->Output == accepted.FindProduct(ROOM_B, TREE_LAYER)->Output);

    RequireCleanCookAfterPublishing(coordinator, fixture, candidate);
    // Every placement keeps its identity and values; only B's edited span changes its instances.
    const ManifestContents edited = ReadContents(coordinator.GetAccepted());
    RequireSameContents(ManifestContents{acceptedContents.Placements, {}}, ManifestContents{edited.Placements, {}});
    for (const AssemblyInstance& instance : acceptedContents.Instances)
    {
        if (instance.Identity.Scope == ROOM_A || instance.Identity.Span == fixture.RoomBSpans[0].Id)
        {
            bool isKept = false;
            for (const AssemblyInstance& current : edited.Instances)
            {
                isKept = isKept == true || (current.Identity == instance.Identity &&
                                            current.Module == instance.Module && current.Offset == instance.Offset);
            }
            REQUIRE(isKept == true);
        }
    }
}

TEST_CASE("A shared-input edit invalidates each dependent output as it is requested",
          "[procedural][generation][demand]")
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    GenerationCoordinator coordinator(store);
    DemandFixture fixture;
    fixture.Rebuild();
    PublishPlan(coordinator, fixture);
    const GenerationManifest accepted = coordinator.GetAccepted();
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;

    SECTION("a module width edit changes the catalog and both assemblies")
    {
        fixture.Modules[1].Width = 35;
        fixture.Rebuild();
        REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_A, trace, candidate) == GenerationStatus::Success);
        REQUIRE(FindRecord(trace, SHARED_SCOPE, CATALOG_STAGE).Outcome == GenerationStageOutcome::Executed);
        REQUIRE(FindRecord(trace, ROOM_A, ASSEMBLY_STAGE).Outcome == GenerationStageOutcome::Executed);
        REQUIRE(FindRecord(trace, ROOM_A, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
        REQUIRE(FindRecord(trace, ROOM_A, STONE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
        REQUIRE(HasScopeRecord(trace, ROOM_B) == false);
        REQUIRE(candidate.FindProduct(ROOM_B, ASSEMBLY_STAGE)->State == GenerationProductState::Pending);
        REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);

        trace.Clear();
        REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_B, trace, candidate) == GenerationStatus::Success);
        REQUIRE(trace.GetRecords().GetSize() == 2);
        REQUIRE(FindRecord(trace, ROOM_B, ASSEMBLY_STAGE).Outcome == GenerationStageOutcome::Executed);
        REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
        for (const uint32 room : {ROOM_A, ROOM_B})
        {
            REQUIRE(candidate.FindProduct(room, ASSEMBLY_STAGE)->Key !=
                    accepted.FindProduct(room, ASSEMBLY_STAGE)->Key);
        }
        RequireCleanCookAfterPublishing(coordinator, fixture, candidate);
    }
    SECTION("reordering the library reruns the catalog but no dependent")
    {
        const AssemblyModule first = fixture.Modules[0];
        fixture.Modules[0] = fixture.Modules[3];
        fixture.Modules[3] = first;
        fixture.Rebuild();
        REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_A, trace, candidate) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_B, trace, candidate) == GenerationStatus::Success);
        const GenerationStageRecord& catalog = FindRecord(trace, SHARED_SCOPE, CATALOG_STAGE);
        REQUIRE(catalog.Outcome == GenerationStageOutcome::Executed);
        REQUIRE(catalog.Output == accepted.FindProduct(SHARED_SCOPE, CATALOG_STAGE)->Output);
        REQUIRE(CountOutcome(trace, GenerationStageOutcome::Executed) == 1);
        REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == fixture.Plan.GetSize() - 1);
        RequireCleanCookAfterPublishing(coordinator, fixture, candidate);
    }
}
