// Cache recovery runs of the two-scope demand fixture over the persistent store:
// a reopened store serves every stage, and damaged entries are rebuilt as each
// scope is requested, with exact manifest equivalence and stable placement and
// instance identities. See
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include "generation_fixture.h"

#include <ludus/content/procedural/storage/file_generation_cache.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace ludus::content::procedural;
using namespace ludus::content::procedural::generation_fixture;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

namespace
{
constexpr usize ENTRY_BUDGET = 1U << 20U;

[[nodiscard]] std::string GetName(const FileGenerationCacheName& name)
{
    return std::string(name.GetView());
}

struct CacheDirectory final
{
    char Root[64] = "/tmp/ludus-generation-recovery-XXXXXX";
    CacheDirectory()
    {
        REQUIRE(::mkdtemp(Root) != nullptr);
    }
    // Best-effort cleanup: only std::filesystem::path allocation can throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~CacheDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
    CacheDirectory(const CacheDirectory&) = delete;
    CacheDirectory& operator=(const CacheDirectory&) = delete;

    [[nodiscard]] std::filesystem::path GetRecord(const GenerationDigest& key) const
    {
        return std::filesystem::path(Root) / GetName(GetFileGenerationCacheRecordName(key));
    }
    [[nodiscard]] std::filesystem::path FindObject(const GenerationDigest& key) const
    {
        const std::string prefix =
            GetName(GetFileGenerationCacheRecordName(key)).substr(0, 2 * GENERATION_DIGEST_BYTES);
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Root))
        {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix + "-") == true && name.ends_with(".object") == true)
            {
                return entry.path();
            }
        }
        FAIL("no object file for the key");
        return {};
    }
};

void FlipLastByte(const std::filesystem::path& path)
{
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.good() == true);
    file.seekg(-1, std::ios::end);
    char last = 0;
    file.get(last);
    file.seekp(-1, std::ios::end);
    file.put(static_cast<char>(last ^ 1));
    file.close();
    REQUIRE(file.good() == true);
}

[[nodiscard]] const GenerationDigest& GetKey(const GenerationManifest& manifest, uint32 scope, uint32 stage)
{
    const GenerationProduct* productOrNull = manifest.FindProduct(scope, stage);
    REQUIRE(productOrNull != nullptr);
    return productOrNull->Key;
}
} // namespace

TEST_CASE("A persistent cache recovers damaged entries to the same manifest and identities",
          "[procedural][generation][storage]")
{
    CacheDirectory directory;
    DemandFixture fixture;
    fixture.Rebuild();
    GenerationManifest accepted;
    {
        FileGenerationCacheStore store;
        REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
        GenerationCoordinator coordinator(store);
        GenerationTrace trace(TRACE_CAPACITY);
        GenerationCandidate candidate;
        REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestAll(fixture.GetPlan(), trace, candidate) == GenerationStatus::Success);
        for (const GenerationStageRecord& record : trace.GetRecords())
        {
            REQUIRE(record.Outcome == GenerationStageOutcome::Executed);
            REQUIRE(record.IsStored == true);
        }
        REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::Success);
        accepted = coordinator.GetAccepted();
    }
    const GenerationManifest clean = CookClean(fixture.GetPlan());
    RequireSameProducts({clean.Products.GetData(), clean.Products.GetSize()},
                        {accepted.Products.GetData(), accepted.Products.GetSize()});
    const ManifestContents acceptedContents = ReadContents(accepted);

    // A reopened store serves every stage of a fresh session.
    FileGenerationCacheStore reopened;
    REQUIRE(reopened.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    GenerationCoordinator coordinator(reopened);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestAll(fixture.GetPlan(), trace, candidate) == GenerationStatus::Success);
    REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == fixture.Plan.GetSize());
    RequireSameProducts({accepted.Products.GetData(), accepted.Products.GetSize()},
                        {candidate.Products.GetData(), candidate.Products.GetSize()});

    // Damage the shared catalog's object, A's assembly record and B's tree layer object, and stage an orphan object
    // no record names, as an interrupted write leaves.
    REQUIRE(std::filesystem::remove(directory.FindObject(GetKey(accepted, SHARED_SCOPE, CATALOG_STAGE))) == true);
    std::filesystem::resize_file(directory.GetRecord(GetKey(accepted, ROOM_A, ASSEMBLY_STAGE)), 1);
    FlipLastByte(directory.FindObject(GetKey(accepted, ROOM_B, TREE_LAYER)));
    const uint8 orphan[] = {1, 2, 3};
    std::ofstream(std::filesystem::path(directory.Root) / "orphan.object", std::ios::binary)
        .write(reinterpret_cast<const char*>(orphan), sizeof(orphan));

    REQUIRE(coordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
    trace.Clear();
    REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_A, trace, candidate) == GenerationStatus::Success);
    for (const uint32 stage : {CATALOG_STAGE, ASSEMBLY_STAGE})
    {
        const uint32 scope = stage == CATALOG_STAGE ? SHARED_SCOPE : ROOM_A;
        REQUIRE(FindRecord(trace, scope, stage).Cache == GenerationCacheOutcome::Corrupt);
        REQUIRE(FindRecord(trace, scope, stage).Outcome == GenerationStageOutcome::Executed);
        REQUIRE(FindRecord(trace, scope, stage).IsStored == true);
    }
    REQUIRE(FindRecord(trace, ROOM_A, TREE_LAYER).Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(HasScopeRecord(trace, ROOM_B) == false);

    trace.Clear();
    REQUIRE(coordinator.TryRequestScope(fixture.GetPlan(), ROOM_B, trace, candidate) == GenerationStatus::Success);
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Cache == GenerationCacheOutcome::Corrupt);
    REQUIRE(FindRecord(trace, ROOM_B, TREE_LAYER).Outcome == GenerationStageOutcome::Executed);
    REQUIRE(FindRecord(trace, ROOM_B, ASSEMBLY_STAGE).Outcome == GenerationStageOutcome::CacheHit);

    // The recovered manifest is exactly the clean cook, with every placement and instance identity unchanged.
    RequireSameProducts({clean.Products.GetData(), clean.Products.GetSize()},
                        {candidate.Products.GetData(), candidate.Products.GetSize()});
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.GetAccepted().Digest == clean.Digest);
    RequireSameContents(acceptedContents, ReadContents(coordinator.GetAccepted()));

    // The rebuilt entries serve the next session again.
    FileGenerationCacheStore third;
    REQUIRE(third.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    GenerationCoordinator thirdCoordinator(third);
    trace.Clear();
    REQUIRE(thirdCoordinator.TryBeginCandidate(fixture.GetPlan(), candidate) == GenerationStatus::Success);
    REQUIRE(thirdCoordinator.TryRequestAll(fixture.GetPlan(), trace, candidate) == GenerationStatus::Success);
    REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == fixture.Plan.GetSize());
}

TEST_CASE("The scatter coordinator stages through a persistent cache", "[procedural][generation][storage]")
{
    CacheDirectory directory;
    FileGenerationCacheStore store;
    REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    ScatterGenerationCoordinator coordinator(store);
    TwoScopeFixture fixture;
    GenerationTrace trace(TRACE_CAPACITY);
    ScatterCandidate first;
    REQUIRE(coordinator.TryStage(fixture.Input, trace, first) == GenerationStatus::Success);
    ScatterCandidate second;
    trace.Clear();
    REQUIRE(coordinator.TryStage(fixture.Input, trace, second) == GenerationStatus::Success);
    REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == trace.GetRecords().GetSize());
    REQUIRE(second.Manifest.Digest == first.Manifest.Digest);
    REQUIRE(second.Manifest.Placements.GetSize() == first.Manifest.Placements.GetSize());
    for (usize i = 0; i < first.Manifest.Placements.GetSize(); ++i)
    {
        REQUIRE(AreEqual(second.Manifest.Placements[i], first.Manifest.Placements[i]) == true);
    }
}
