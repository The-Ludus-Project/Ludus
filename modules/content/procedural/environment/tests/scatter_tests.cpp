// Scatter kernel contracts: identity packing, validation, the golden placement
// digest, independence from chunking/order/workers, property isolation and
// failure preservation. See docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter.

#include <ludus/content/procedural/environment/scatter.h>

#include "scatter_golden.h"

#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::procedural;
using namespace ludus::content::procedural::golden;
using ludus::foundation::BorrowedView;
using ludus::foundation::StableSort;
using ludus::foundation::core::int64;

namespace threading = ludus::foundation::threading;

namespace
{
constexpr usize MAX_TEST_PLACEMENTS = 4096;
/// The golden recipe; tests mutate their own instance.
struct Fixture final : GoldenScatter
{
    Fixture()
    {
        REQUIRE(InitializeGoldenScatter(*this) == true);
    }
};

/// Generated placements of one call, owned by the test.
struct Result final
{
    ScatterPlacement Placements[MAX_TEST_PLACEMENTS] = {};
    usize Count = 0;
};

[[nodiscard]] ScatterStatus
Generate(const ScatterRecipe& recipe, const ScatterKeys& keys, usize layer, ScatterCellRange cells, Result& outResult)
{
    return TryGenerateScatter(recipe,
                              keys,
                              layer,
                              cells,
                              BorrowedView<ScatterPlacement>(outResult.Placements, MAX_TEST_PLACEMENTS),
                              outResult.Count);
}

[[nodiscard]] ScatterCellRange GetCells(const ScatterRecipe& recipe, usize layer)
{
    ScatterCellRange cells;
    REQUIRE(TryGetScatterCells(recipe, layer, cells) == ScatterStatus::Success);
    return cells;
}

/// Canonical order: cell Y, cell X, candidate.
[[nodiscard]] bool IsCanonicallyBefore(const ScatterPlacement& a, const ScatterPlacement& b) noexcept
{
    if (a.Identity.CellY != b.Identity.CellY)
    {
        return a.Identity.CellY < b.Identity.CellY;
    }
    if (a.Identity.CellX != b.Identity.CellX)
    {
        return a.Identity.CellX < b.Identity.CellX;
    }
    return a.Identity.Candidate < b.Identity.Candidate;
}

[[nodiscard]] bool AreEqual(const ScatterPlacement& a, const ScatterPlacement& b) noexcept
{
    return a.Identity == b.Identity && a.LatticeX == b.LatticeX && a.LatticeY == b.LatticeY && a.Variant == b.Variant &&
           a.Rotation == b.Rotation && a.Scale == b.Scale;
}

void RequireSameResult(const Result& expected, const Result& actual)
{
    REQUIRE(actual.Count == expected.Count);
    for (usize i = 0; i < expected.Count; ++i)
    {
        REQUIRE(AreEqual(actual.Placements[i], expected.Placements[i]) == true);
    }
}

[[nodiscard]] bool Contains(const ScatterRectangle& rectangle, int32 x, int32 y) noexcept
{
    return x >= rectangle.MinimumX && x < rectangle.MaximumX && y >= rectangle.MinimumY && y < rectangle.MaximumY;
}

/// How GenerateTiled splits a layer: tile size in cells, and whether tiles are visited in reverse.
struct Tiling final
{
    int32 Width = 1;
    int32 Height = 1;
    bool IsReversed = false;
};

/// Generate a layer as a grid of tiles, optionally visiting tiles in reverse, then sort into canonical order.
void GenerateTiled(const Fixture& fixture, usize layer, Tiling tiling, Result& outResult)
{
    const ScatterCellRange cells = GetCells(fixture.Recipe, layer);
    const int32 tilesX = (cells.MaximumX - cells.MinimumX + tiling.Width - 1) / tiling.Width;
    const int32 tilesY = (cells.MaximumY - cells.MinimumY + tiling.Height - 1) / tiling.Height;
    const int32 tileCount = tilesX * tilesY;
    outResult.Count = 0;
    for (int32 visit = 0; visit < tileCount; ++visit)
    {
        const int32 tile = tiling.IsReversed == true ? tileCount - 1 - visit : visit;
        ScatterCellRange range;
        range.MinimumX = cells.MinimumX + (tile % tilesX) * tiling.Width;
        range.MinimumY = cells.MinimumY + (tile / tilesX) * tiling.Height;
        range.MaximumX =
            range.MinimumX + tiling.Width < cells.MaximumX ? range.MinimumX + tiling.Width : cells.MaximumX;
        range.MaximumY =
            range.MinimumY + tiling.Height < cells.MaximumY ? range.MinimumY + tiling.Height : cells.MaximumY;
        Result piece;
        REQUIRE(Generate(fixture.Recipe, fixture.Keys, layer, range, piece) == ScatterStatus::Success);
        REQUIRE(outResult.Count + piece.Count <= MAX_TEST_PLACEMENTS);
        for (usize i = 0; i < piece.Count; ++i)
        {
            outResult.Placements[outResult.Count] = piece.Placements[i];
            ++outResult.Count;
        }
    }
    static Result scratch;
    REQUIRE(StableSort(outResult.Placements, scratch.Placements, outResult.Count, IsCanonicallyBefore) == true);
}

/// One job's share of a layer: a band of cell rows.
struct BandJob final
{
    const Fixture* FixtureOrNull = nullptr;
    usize Layer = 0;
    ScatterCellRange Cells;
    Result Output;
};

threading::JobOutcome RunBand(void* context) noexcept
{
    BandJob& job = *static_cast<BandJob*>(context);
    if (job.FixtureOrNull == nullptr)
    {
        return threading::JobOutcome::Failed;
    }
    const ScatterStatus status =
        Generate(job.FixtureOrNull->Recipe, job.FixtureOrNull->Keys, job.Layer, job.Cells, job.Output);
    return status == ScatterStatus::Success ? threading::JobOutcome::Succeeded : threading::JobOutcome::Failed;
}
} // namespace

TEST_CASE("Scatter identities pack injectively into the address scope", "[procedural][scatter]")
{
    constexpr ScatterIdentity EXTREME = {0xffffu, 0xffu, 0xffu, -32768, 32767};
    static_assert(UnpackScatterScope(PackScatterScope(EXTREME)) == EXTREME);
    static_assert(PackScatterScope({1u, 0u, 0u, 0, 0}) == 0x0001000000000000ULL);
    static_assert(PackScatterScope({0u, 0u, 0u, -1, 0}) == 0x00000000ffff0000ULL);

    const ScatterIdentity identities[] = {
        {1u, 3u, 0u, 0, 0},
        {1u, 3u, 0u, 0, -1},
        {1u, 3u, 0u, -1, 0},
        {1u, 3u, 1u, 0, 0},
        {1u, 4u, 0u, 0, 0},
        {2u, 3u, 0u, 0, 0},
    };
    for (const ScatterIdentity& a : identities)
    {
        CHECK(UnpackScatterScope(PackScatterScope(a)) == a);
        for (const ScatterIdentity& b : identities)
        {
            CHECK((PackScatterScope(a) == PackScatterScope(b)) == (a == b));
        }
    }
}

TEST_CASE("Scatter keys require distinct game-range domains", "[procedural][scatter]")
{
    ScatterKeys keys;
    keys.Placement.Value = 1;
    const ScatterDomains invalid[] = {
        {0u, 12u, 13u},
        {11u, 11u, 13u},
        {11u, 12u, 0x42414B45u},
    };
    for (const ScatterDomains& domains : invalid)
    {
        CHECK(TryMakeScatterKeys(WORLD_SEED, domains, keys) == ScatterStatus::InvalidRecipe);
        CHECK(keys.Placement.Value == 1);
    }
    REQUIRE(TryMakeScatterKeys(WORLD_SEED, DOMAINS, keys) == ScatterStatus::Success);
    CHECK(keys.Placement.Value != keys.Variant.Value);
    CHECK(keys.Variant.Value != keys.Transform.Value);
}

TEST_CASE("Scatter validation reports the first problem", "[procedural][scatter]")
{
    Fixture fixture;
    REQUIRE(ValidateScatterRecipe(fixture.Recipe).Status == ScatterStatus::Success);

    struct Case final
    {
        void (*Break)(Fixture& inoutFixture) noexcept;
        ScatterRecipeIssue Issue;
        usize Index;
    };
    static const ScatterRectangle EMPTY_KEEP_OUT[] = {{0, 0, 10, 10}, {5, 5, 5, 9}};
    static const ScatterVariant WIDE_VARIANT[] = {{41u}};
    const Case cases[] = {
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Recipe.Room = 0; }, ScatterRecipeIssue::RoomZero, 0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Recipe.Region.MaximumX = inoutFixture.Recipe.Region.MinimumX;
         },
         ScatterRecipeIssue::EmptyRegion,
         0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Recipe.KeepOuts = BorrowedView<const ScatterRectangle>(nullptr, 1);
         },
         ScatterRecipeIssue::KeepOutCount,
         0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Recipe.KeepOuts = BorrowedView<const ScatterRectangle>(EMPTY_KEEP_OUT);
         },
         ScatterRecipeIssue::EmptyKeepOut,
         1},
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Recipe.Layers = {}; }, ScatterRecipeIssue::LayerCount, 0},
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Layers[1].Id = inoutFixture.Layers[0].Id; },
         ScatterRecipeIssue::DuplicateLayer,
         1},
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Layers[1].CellSize = 0; }, ScatterRecipeIssue::CellSize, 1},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Layers[0].CellSize = 1;
             inoutFixture.Recipe.Region = {-32769, 0, 10, 10};
         },
         ScatterRecipeIssue::CellRange,
         0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Layers[0].CellSize = 1;
             inoutFixture.Recipe.Region = {0, 0, 10, 32769};
         },
         ScatterRecipeIssue::CellRange,
         0},
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Layers[0].CandidatesPerCell = 0; },
         ScatterRecipeIssue::CandidateCount,
         0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Layers[0].CandidatesPerCell = MAX_SCATTER_CANDIDATES_PER_CELL + 1;
         },
         ScatterRecipeIssue::CandidateCount,
         0},
        {[](Fixture& inoutFixture) noexcept { inoutFixture.Layers[1].Variants = {}; },
         ScatterRecipeIssue::VariantCount,
         1},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Layers[0].Variants = BorrowedView<const ScatterVariant>(WIDE_VARIANT);
         },
         ScatterRecipeIssue::ExclusionRadius,
         0},
        {[](Fixture& inoutFixture) noexcept {
             inoutFixture.Layers[1].MaximumScale = static_cast<uint16>(inoutFixture.Layers[1].MinimumScale - 1);
         },
         ScatterRecipeIssue::ScaleRange,
         1},
    };
    for (const Case& testCase : cases)
    {
        Fixture broken;
        testCase.Break(broken);
        const ScatterValidation validation = ValidateScatterRecipe(broken.Recipe);
        CHECK(validation.Status == ScatterStatus::InvalidRecipe);
        CHECK(validation.Issue == testCase.Issue);
        CHECK(validation.Index == testCase.Index);

        // Every entry point rejects the invalid recipe before writing.
        ScatterCellRange cells = {1, 2, 3, 4};
        CHECK(TryGetScatterCells(broken.Recipe, 0, cells) == ScatterStatus::InvalidRecipe);
        CHECK(cells.MinimumX == 1);
    }

    // The widest representable cell range is accepted: cells -32768 through 32767 at cell size one, with
    // radius-free variants because an exclusion radius may not exceed the cell size.
    static const ScatterVariant POINT_VARIANT[] = {{0u}};
    Fixture widest;
    for (ScatterLayer& layer : widest.Layers)
    {
        layer.CellSize = 1;
        layer.Variants = BorrowedView<const ScatterVariant>(POINT_VARIANT);
    }
    widest.Recipe.Region = {-32768, -32768, 32768, 32768};
    CHECK(ValidateScatterRecipe(widest.Recipe).Status == ScatterStatus::Success);
    const ScatterCellRange cells = GetCells(widest.Recipe, 0);
    CHECK(cells.MinimumX == -32768);
    CHECK(cells.MaximumY == 32768);
}

TEST_CASE("Scatter output matches the golden digest and its invariants", "[procedural][scatter]")
{
    const Fixture fixture;
    PlacementDigest digest;
    usize totalCount = 0;
    for (usize layer = 0; layer < fixture.Recipe.Layers.GetSize(); ++layer)
    {
        static Result result;
        REQUIRE(Generate(fixture.Recipe, fixture.Keys, layer, GetCells(fixture.Recipe, layer), result) ==
                ScatterStatus::Success);
        REQUIRE(result.Count > 0);
        const ScatterLayer& recipeLayer = fixture.Layers[layer];
        for (usize i = 0; i < result.Count; ++i)
        {
            const ScatterPlacement& placement = result.Placements[i];
            digest.Add(placement);
            CHECK(placement.Identity.Room == fixture.Recipe.Room);
            CHECK(placement.Identity.Layer == recipeLayer.Id);
            CHECK(placement.Identity.Candidate < recipeLayer.CandidatesPerCell);
            CHECK(Contains(fixture.Recipe.Region, placement.LatticeX, placement.LatticeY) == true);
            for (const ScatterRectangle& keepOut : KEEP_OUTS)
            {
                CHECK(Contains(keepOut, placement.LatticeX, placement.LatticeY) == false);
            }
            CHECK(placement.Variant < recipeLayer.Variants.GetSize());
            CHECK(placement.Scale >= recipeLayer.MinimumScale);
            CHECK(placement.Scale <= recipeLayer.MaximumScale);
            if (i > 0)
            {
                CHECK(IsCanonicallyBefore(result.Placements[i - 1], placement) == true);
            }
            // Accepted placements never conflict with one another.
            for (usize j = 0; j < i; ++j)
            {
                const ScatterPlacement& other = result.Placements[j];
                const int64 dx = static_cast<int64>(placement.LatticeX) - other.LatticeX;
                const int64 dy = static_cast<int64>(placement.LatticeY) - other.LatticeY;
                const int64 reach = static_cast<int64>(recipeLayer.Variants[placement.Variant].ExclusionRadius) +
                                    recipeLayer.Variants[other.Variant].ExclusionRadius;
                CHECK(dx * dx + dy * dy >= reach * reach);
            }
        }
        totalCount += result.Count;
    }
    // Pinned answers for SCATTER_LAYOUT_VERSION 1. A change here changes every saved world's placements and
    // requires a new layout version, never an updated constant.
    CHECK(totalCount == PLACEMENT_COUNT);
    CHECK(digest.GetValue() == PLACEMENT_DIGEST);

    // The shared helper the WebAssembly probe runs agrees with this test.
    static ScatterPlacement storage[STORAGE_PLACEMENTS];
    const GoldenDigest shared = ComputeGoldenDigest(fixture, BorrowedView<ScatterPlacement>(storage));
    REQUIRE(shared.Status == ScatterStatus::Success);
    CHECK(shared.Count == PLACEMENT_COUNT);
    CHECK(shared.Value == PLACEMENT_DIGEST);
}

TEST_CASE("Scatter acceptance is independent of chunking and traversal order", "[procedural][scatter]")
{
    const Fixture fixture;
    for (usize layer = 0; layer < fixture.Recipe.Layers.GetSize(); ++layer)
    {
        static Result whole;
        REQUIRE(Generate(fixture.Recipe, fixture.Keys, layer, GetCells(fixture.Recipe, layer), whole) ==
                ScatterStatus::Success);
        const Tiling tilings[] = {{1, 1, false}, {1, 1, true}, {3, 2, false}, {2, 5, true}, {1000, 1, false}};
        for (const Tiling& tiling : tilings)
        {
            static Result tiled;
            GenerateTiled(fixture, layer, tiling, tiled);
            RequireSameResult(whole, tiled);
        }
    }
}

TEST_CASE("Scatter results are identical for every worker count and band size", "[procedural][scatter]")
{
    const Fixture fixture;
    constexpr usize LAYER = 0;
    const ScatterCellRange cells = GetCells(fixture.Recipe, LAYER);
    static Result whole;
    REQUIRE(Generate(fixture.Recipe, fixture.Keys, LAYER, cells, whole) == ScatterStatus::Success);

    constexpr usize MAX_BANDS = 16;
    static BandJob jobs[MAX_BANDS];
    const uint32 workerCounts[] = {0u, 1u, 2u, 4u};
    const int32 bandHeights[] = {1, 3, 1000};
    for (const uint32 workerCount : workerCounts)
    {
        threading::JobSystem system;
        threading::JobSystemConfig config;
        config.WorkerCount = workerCount;
        REQUIRE(system.Initialize(config) == threading::JobStatus::Ok);
        for (const int32 bandHeight : bandHeights)
        {
            threading::JobGraph graph;
            REQUIRE(graph.Initialize() == threading::JobStatus::Ok);
            usize bandCount = 0;
            for (int32 y = cells.MinimumY; y < cells.MaximumY; y += bandHeight)
            {
                REQUIRE(bandCount < MAX_BANDS);
                BandJob& job = jobs[bandCount];
                job.FixtureOrNull = &fixture;
                job.Layer = LAYER;
                job.Cells = {cells.MinimumX,
                             y,
                             cells.MaximumX,
                             y + bandHeight < cells.MaximumY ? y + bandHeight : cells.MaximumY};
                job.Output.Count = 0;
                threading::JobHandle handle;
                REQUIRE(graph.Add(RunBand, &job, handle) == threading::JobStatus::Ok);
                ++bandCount;
            }
            REQUIRE(graph.Seal() == threading::JobStatus::Ok);
            REQUIRE(system.Submit(graph) == threading::JobStatus::Ok);
            threading::JobOutcome outcome = threading::JobOutcome::Pending;
            REQUIRE(system.Wait(outcome) == threading::JobStatus::Ok);
            REQUIRE(outcome == threading::JobOutcome::Succeeded);

            // Bands cover rows in ascending order, so joining them in band order is canonical.
            static Result joined;
            joined.Count = 0;
            for (usize band = 0; band < bandCount; ++band)
            {
                for (usize i = 0; i < jobs[band].Output.Count; ++i)
                {
                    joined.Placements[joined.Count] = jobs[band].Output.Placements[i];
                    ++joined.Count;
                }
            }
            RequireSameResult(whole, joined);
        }
        REQUIRE(system.Shutdown() == threading::JobStatus::Ok);
    }
}

TEST_CASE("Scatter properties are isolated from one another", "[procedural][scatter]")
{
    const Fixture fixture;
    static Result baseline;
    REQUIRE(Generate(fixture.Recipe, fixture.Keys, 0, GetCells(fixture.Recipe, 0), baseline) == ScatterStatus::Success);

    SECTION("Transform ranges and keys move nothing")
    {
        Fixture changed;
        changed.Layers[0].MinimumScale = 10;
        changed.Layers[0].MaximumScale = 20;
        ScatterDomains domains = DOMAINS;
        domains.Transform = 99u;
        REQUIRE(TryMakeScatterKeys(WORLD_SEED, domains, changed.Keys) == ScatterStatus::Success);
        static Result result;
        REQUIRE(Generate(changed.Recipe, changed.Keys, 0, GetCells(changed.Recipe, 0), result) ==
                ScatterStatus::Success);
        REQUIRE(result.Count == baseline.Count);
        bool hasDifferentRotation = false;
        for (usize i = 0; i < baseline.Count; ++i)
        {
            CHECK(result.Placements[i].Identity == baseline.Placements[i].Identity);
            CHECK(result.Placements[i].LatticeX == baseline.Placements[i].LatticeX);
            CHECK(result.Placements[i].LatticeY == baseline.Placements[i].LatticeY);
            CHECK(result.Placements[i].Variant == baseline.Placements[i].Variant);
            CHECK(result.Placements[i].Scale >= 10);
            CHECK(result.Placements[i].Scale <= 20);
            hasDifferentRotation =
                hasDifferentRotation || result.Placements[i].Rotation != baseline.Placements[i].Rotation;
        }
        CHECK(hasDifferentRotation == true);
    }

    SECTION("Variant radii change acceptance but not candidate positions")
    {
        static const ScatterVariant POINT_VARIANTS[] = {{0u}, {0u}, {0u}};
        Fixture changed;
        changed.Layers[0].Variants = BorrowedView<const ScatterVariant>(POINT_VARIANTS);
        static Result result;
        REQUIRE(Generate(changed.Recipe, changed.Keys, 0, GetCells(changed.Recipe, 0), result) ==
                ScatterStatus::Success);
        // Without exclusion every eligible candidate is accepted, so the baseline is a subset at the same positions.
        CHECK(result.Count > baseline.Count);
        usize matched = 0;
        for (usize i = 0; i < result.Count && matched < baseline.Count; ++i)
        {
            if (result.Placements[i].Identity == baseline.Placements[matched].Identity)
            {
                CHECK(result.Placements[i].LatticeX == baseline.Placements[matched].LatticeX);
                CHECK(result.Placements[i].LatticeY == baseline.Placements[matched].LatticeY);
                CHECK(result.Placements[i].Variant == baseline.Placements[matched].Variant);
                ++matched;
            }
        }
        CHECK(matched == baseline.Count);
    }

    SECTION("Adding or editing another layer leaves this layer unchanged")
    {
        static const ScatterVariant EXTRA_VARIANTS[] = {{5u}, {8u}};
        Fixture changed;
        ScatterLayer layers[3] = {changed.Layers[0], changed.Layers[1], changed.Layers[1]};
        layers[1].CandidatesPerCell = 7;
        layers[2].Id = 200;
        layers[2].Variants = BorrowedView<const ScatterVariant>(EXTRA_VARIANTS);
        changed.Recipe.Layers = BorrowedView<const ScatterLayer>(layers);
        static Result result;
        REQUIRE(Generate(changed.Recipe, changed.Keys, 0, GetCells(changed.Recipe, 0), result) ==
                ScatterStatus::Success);
        RequireSameResult(baseline, result);
    }

    SECTION("Another room or world seed gives different placements")
    {
        Fixture otherRoom;
        otherRoom.Recipe.Room = 8;
        static Result roomResult;
        REQUIRE(Generate(otherRoom.Recipe, otherRoom.Keys, 0, GetCells(otherRoom.Recipe, 0), roomResult) ==
                ScatterStatus::Success);
        Fixture otherSeed;
        REQUIRE(TryMakeScatterKeys(WORLD_SEED + 1u, DOMAINS, otherSeed.Keys) == ScatterStatus::Success);
        static Result seedResult;
        REQUIRE(Generate(otherSeed.Recipe, otherSeed.Keys, 0, GetCells(otherSeed.Recipe, 0), seedResult) ==
                ScatterStatus::Success);
        const usize firstCount = roomResult.Count < baseline.Count ? roomResult.Count : baseline.Count;
        bool isRoomDifferent = roomResult.Count != baseline.Count;
        bool isSeedDifferent = seedResult.Count != baseline.Count;
        for (usize i = 0; i < firstCount; ++i)
        {
            isRoomDifferent = isRoomDifferent || roomResult.Placements[i].LatticeX != baseline.Placements[i].LatticeX;
        }
        for (usize i = 0; i < seedResult.Count && i < baseline.Count; ++i)
        {
            isSeedDifferent = isSeedDifferent || seedResult.Placements[i].LatticeX != baseline.Placements[i].LatticeX;
        }
        CHECK(isRoomDifferent == true);
        CHECK(isSeedDifferent == true);
    }
}

TEST_CASE("Scatter rejects bad ranges and storage before writing", "[procedural][scatter]")
{
    const Fixture fixture;
    const ScatterCellRange cells = GetCells(fixture.Recipe, 0);
    usize candidateCount = 0;
    REQUIRE(TryCountScatterCandidates(fixture.Recipe, 0, cells, candidateCount) == ScatterStatus::Success);
    CHECK(candidateCount == static_cast<usize>((cells.MaximumX - cells.MinimumX) * (cells.MaximumY - cells.MinimumY) *
                                               fixture.Layers[0].CandidatesPerCell));

    static Result result;
    ScatterPlacement sentinel;
    sentinel.LatticeX = 12345;
    for (ScatterPlacement& placement : result.Placements)
    {
        placement = sentinel;
    }
    result.Count = 77;
    const ScatterCellRange invalidRanges[] = {
        {cells.MinimumX, cells.MinimumY, cells.MinimumX, cells.MaximumY},
        {cells.MinimumX - 1, cells.MinimumY, cells.MaximumX, cells.MaximumY},
        {cells.MinimumX, cells.MinimumY, cells.MaximumX, cells.MaximumY + 1},
    };
    for (const ScatterCellRange& range : invalidRanges)
    {
        CHECK(Generate(fixture.Recipe, fixture.Keys, 0, range, result) == ScatterStatus::InvalidRange);
    }
    CHECK(Generate(fixture.Recipe, fixture.Keys, 2, cells, result) == ScatterStatus::InvalidRecipe);
    CHECK(TryGenerateScatter(fixture.Recipe,
                             fixture.Keys,
                             0,
                             cells,
                             BorrowedView<ScatterPlacement>(result.Placements, candidateCount - 1),
                             result.Count) == ScatterStatus::CapacityExceeded);
    CHECK(TryGenerateScatter(fixture.Recipe,
                             fixture.Keys,
                             0,
                             cells,
                             BorrowedView<ScatterPlacement>(nullptr, candidateCount),
                             result.Count) == ScatterStatus::CapacityExceeded);
    CHECK(result.Count == 77);
    for (const ScatterPlacement& placement : result.Placements)
    {
        CHECK(placement.LatticeX == 12345);
    }
}
