// Page set publication: validated replacement, retained previous generations and stale candidates.

#include <ludus/content/foliage_pages/foliage_page_publication.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>

#include "foliage_pages_golden.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::foliage_pages;
namespace format = ludus::content::foliage_format;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

constexpr usize PLACEMENT_COUNT = sizeof(golden::PLACEMENTS) / sizeof(golden::PLACEMENTS[0]);

/// Encode the golden page set into owned bytes.
Array<uint8> EncodeGolden()
{
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(golden::ENCODED_BYTES) == true);
    usize written = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(WriteFoliagePageSet(golden::MakeDescription(),
                                {},
                                BorrowedView<uint8>(bytes.GetData(), bytes.GetSize()),
                                written,
                                diagnostic) == FoliagePageStatus::Ok);
    REQUIRE(written == golden::ENCODED_BYTES);
    return bytes;
}

/// Stage owned bytes and require success.
FoliagePageSetCandidate StageReady(const FoliagePageSetPublication& publication, Array<uint8> bytes)
{
    FoliagePageSetCandidate candidate;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(publication.TryStage(bytes, golden::BINDING, candidate, diagnostic) == FoliagePageStatus::Ok);
    REQUIRE(candidate.GetState() == FoliagePageSetCandidateState::Ready);
    REQUIRE(bytes.IsEmpty() == true);
    return candidate;
}

/// Publish the golden page set as the first generation.
void PublishGolden(FoliagePageSetPublication& inoutPublication)
{
    FoliagePageSetCandidate candidate = StageReady(inoutPublication, EncodeGolden());
    REQUIRE(inoutPublication.TryPublish(candidate) == FoliagePublicationStatus::Ok);
}

/// Whether a view reads every golden placement.
bool ReadsGoldenPlacements(const FoliagePageSetView& view)
{
    if (view.GetPlacementCount() != PLACEMENT_COUNT)
    {
        return false;
    }
    for (usize i = 0; i < PLACEMENT_COUNT; ++i)
    {
        FoliagePlacement placement;
        if (view.TryGetPlacement(i, placement) != FoliagePageStatus::Ok ||
            (placement == golden::PLACEMENTS[i]) == false)
        {
            return false;
        }
    }
    return true;
}

/// Require the accepted generation to be the golden page set at a revision.
void RequireAcceptedGolden(const FoliagePageSetPublication& publication, uint64 revision)
{
    CHECK(publication.GetAcceptedRevision() == revision);
    CHECK(publication.GetAcceptedBytes().GetSize() == golden::ENCODED_BYTES);
    CHECK(golden::ComputeDigest(publication.GetAcceptedBytes()) == golden::ENCODED_DIGEST);
    CHECK(ReadsGoldenPlacements(publication.GetAccepted()) == true);
}
} // namespace

TEST_CASE("a published page set becomes the accepted generation", "[foliage][publication]")
{
    FoliagePageSetPublication publication;
    CHECK(publication.GetAcceptedRevision() == 0);
    CHECK(publication.GetAccepted().IsValid() == false);
    CHECK(publication.GetAcceptedBytes().GetSize() == 0);

    FoliagePageSetCandidate first = StageReady(publication, EncodeGolden());
    CHECK(first.GetBaseRevision() == 0);
    CHECK(first.GetDiagnostic().Status == FoliagePageStatus::Ok);
    CHECK(ReadsGoldenPlacements(first.GetView()) == true);
    // Staging never changes the accepted generation.
    CHECK(publication.GetAccepted().IsValid() == false);

    REQUIRE(publication.TryPublish(first) == FoliagePublicationStatus::Ok);
    RequireAcceptedGolden(publication, 1);
    // There was no previous generation to retire.
    CHECK(first.GetState() == FoliagePageSetCandidateState::Empty);
    CHECK(first.GetView().IsValid() == false);
    CHECK(publication.TryPublish(first) == FoliagePublicationStatus::NotReady);

    // A replacement retires the previous generation into the candidate; its views stay readable.
    const FoliagePageSetView previousView = publication.GetAccepted();
    FoliagePageSetCandidate second = StageReady(publication, EncodeGolden());
    CHECK(second.GetBaseRevision() == 1);
    REQUIRE(publication.TryPublish(second) == FoliagePublicationStatus::Ok);
    RequireAcceptedGolden(publication, 2);
    CHECK(second.GetState() == FoliagePageSetCandidateState::Retired);
    CHECK(ReadsGoldenPlacements(second.GetView()) == true);
    CHECK(ReadsGoldenPlacements(previousView) == true);
    CHECK(publication.TryPublish(second) == FoliagePublicationStatus::NotReady);

    // Moving a candidate keeps its views valid and empties the source.
    FoliagePageSetCandidate moved = ludus::foundation::Move(second);
    CHECK(second.GetState() == FoliagePageSetCandidateState::Empty);
    CHECK(moved.GetState() == FoliagePageSetCandidateState::Retired);
    CHECK(ReadsGoldenPlacements(previousView) == true);
    moved.Clear();
    CHECK(moved.GetState() == FoliagePageSetCandidateState::Empty);
    RequireAcceptedGolden(publication, 2);
}

TEST_CASE("failed replacements keep the previous generation", "[foliage][publication]")
{
    FoliagePageLimits limits;
    limits.MaximumBytes = golden::ENCODED_BYTES;
    FoliagePageSetPublication publication(limits);
    PublishGolden(publication);
    RequireAcceptedGolden(publication, 1);

    FoliagePageSetCandidate candidate;
    FoliagePageDiagnostic diagnostic;

    SECTION("malformed bytes")
    {
        Array<uint8> bytes = EncodeGolden();
        bytes[0] ^= 1U;
        REQUIRE(publication.TryStage(bytes, golden::BINDING, candidate, diagnostic) == FoliagePageStatus::Malformed);
        CHECK(diagnostic.Issue == FoliagePageIssue::Magic);
        CHECK(candidate.GetDiagnostic().Issue == FoliagePageIssue::Magic);
        // The caller keeps the offending bytes.
        CHECK(bytes.GetSize() == golden::ENCODED_BYTES);
    }
    SECTION("over the byte budget")
    {
        Array<uint8> bytes = EncodeGolden();
        REQUIRE(bytes.TryResize(golden::ENCODED_BYTES + 1) == true);
        REQUIRE(publication.TryStage(bytes, golden::BINDING, candidate, diagnostic) ==
                FoliagePageStatus::LimitExceeded);
        CHECK(diagnostic.Issue == FoliagePageIssue::Size);
    }
    SECTION("another terrain revision")
    {
        Array<uint8> bytes = EncodeGolden();
        FoliagePageBinding binding = golden::BINDING;
        binding.TerrainRevision += 1;
        REQUIRE(publication.TryStage(bytes, binding, candidate, diagnostic) == FoliagePageStatus::RevisionMismatch);
        CHECK(diagnostic.Issue == FoliagePageIssue::TerrainRevision);
    }
    SECTION("another domain")
    {
        Array<uint8> bytes = EncodeGolden();
        FoliagePageBinding binding = golden::BINDING;
        binding.DomainId += 1;
        REQUIRE(publication.TryStage(bytes, binding, candidate, diagnostic) == FoliagePageStatus::RevisionMismatch);
        CHECK(diagnostic.Issue == FoliagePageIssue::DomainId);
    }
    SECTION("no bytes")
    {
        Array<uint8> bytes;
        CHECK(publication.TryStage(bytes, golden::BINDING, candidate, diagnostic) != FoliagePageStatus::Ok);
    }

    CHECK(candidate.GetState() == FoliagePageSetCandidateState::Failed);
    CHECK(candidate.GetView().IsValid() == false);
    CHECK(candidate.GetDiagnostic().Status == diagnostic.Status);
    CHECK(publication.TryPublish(candidate) == FoliagePublicationStatus::NotReady);
    RequireAcceptedGolden(publication, 1);

    // A failed staging replaces a Ready candidate, so the earlier replacement cannot be published by mistake.
    FoliagePageSetCandidate ready = StageReady(publication, EncodeGolden());
    Array<uint8> corrupt = EncodeGolden();
    corrupt[0] ^= 1U;
    REQUIRE(publication.TryStage(corrupt, golden::BINDING, ready, diagnostic) == FoliagePageStatus::Malformed);
    CHECK(ready.GetState() == FoliagePageSetCandidateState::Failed);
    CHECK(publication.TryPublish(ready) == FoliagePublicationStatus::NotReady);

    // A valid replacement after the failures publishes normally.
    FoliagePageSetCandidate repaired = StageReady(publication, EncodeGolden());
    REQUIRE(publication.TryPublish(repaired) == FoliagePublicationStatus::Ok);
    RequireAcceptedGolden(publication, 2);
}

TEST_CASE("only the first candidate staged from a revision publishes", "[foliage][publication]")
{
    FoliagePageSetPublication publication;
    PublishGolden(publication);

    // Two replacements cooked concurrently from revision 1 complete in either order.
    FoliagePageSetCandidate early = StageReady(publication, EncodeGolden());
    FoliagePageSetCandidate late = StageReady(publication, EncodeGolden());
    REQUIRE(publication.TryPublish(late) == FoliagePublicationStatus::Ok);
    const usize retiredPlacements = late.GetView().GetPlacementCount();
    CHECK(retiredPlacements == PLACEMENT_COUNT);

    CHECK(publication.TryPublish(early) == FoliagePublicationStatus::StaleCandidate);
    // A stale candidate is unchanged, so the caller can still inspect or discard it.
    CHECK(early.GetState() == FoliagePageSetCandidateState::Ready);
    CHECK(early.GetBaseRevision() == 1);
    RequireAcceptedGolden(publication, 2);
    early.Clear();
    CHECK(publication.TryPublish(early) == FoliagePublicationStatus::NotReady);
}

TEST_CASE("candidates belong to the publication that staged them", "[foliage][publication]")
{
    FoliagePageSetPublication first;
    FoliagePageSetPublication second;
    FoliagePageSetCandidate candidate = StageReady(first, EncodeGolden());
    CHECK(second.TryPublish(candidate) == FoliagePublicationStatus::InvalidArgument);
    CHECK(candidate.GetState() == FoliagePageSetCandidateState::Ready);
    CHECK(second.GetAccepted().IsValid() == false);
    CHECK(second.GetAcceptedRevision() == 0);
    CHECK(first.TryPublish(candidate) == FoliagePublicationStatus::Ok);
}

TEST_CASE("publication limits above the format limits reject every candidate", "[foliage][publication]")
{
    FoliagePageLimits limits;
    limits.MaximumBytes = format::MAX_PAGE_SET_BYTES + 1;
    const FoliagePageSetPublication publication(limits);
    CHECK(publication.GetLimits().MaximumBytes == limits.MaximumBytes);
    Array<uint8> bytes = EncodeGolden();
    FoliagePageSetCandidate candidate;
    FoliagePageDiagnostic diagnostic;
    CHECK(publication.TryStage(bytes, golden::BINDING, candidate, diagnostic) == FoliagePageStatus::InvalidArgument);
    CHECK(diagnostic.Issue == FoliagePageIssue::Limits);
    CHECK(candidate.GetState() == FoliagePageSetCandidateState::Failed);
}
