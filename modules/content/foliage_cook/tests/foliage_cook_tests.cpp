// Bounded terrain foliage cook: order independence, terrain rules, strata, suppressions, dirty recook and golden
// bytes.

#include <ludus/content/foliage_cook/foliage_cook.h>

#include <ludus/content/foliage_pages/foliage_page_publication.h>

#include <ludus/content/terrain/terrain_surface.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include "foliage_cook_golden.h"
#include "foliage_pages_golden.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::foliage_cook;
namespace format = ludus::content::foliage_format;
namespace pages = ludus::content::foliage_pages;
namespace terrain = ludus::content::terrain;
namespace threading = ludus::foundation::threading;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::float64;
using ludus::foundation::int16;

constexpr int64 LATTICE_UNITS = format::LATTICE_UNITS_PER_TERRAIN_SAMPLE;
constexpr usize WORKER_COUNT = 4;

/// The golden fixture, loaded once per test.
struct Fixture final
{
    golden::GoldenCook Cook;

    Fixture()
    {
        REQUIRE(golden::TryInitializeGoldenCook(Cook) == true);
    }
};

Array<uint8> CookAll(const FoliageCookInput& input)
{
    Array<uint8> bytes;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(golden::TryCookAll(input, bytes, diagnostic) == FoliageCookStatus::Ok);
    return bytes;
}

Array<FoliagePageAddress> GetTerrainPages(const FoliageCookInput& input)
{
    Array<FoliagePageAddress> addresses;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryGetFoliageTerrainPages(input, addresses, diagnostic) == FoliageCookStatus::Ok);
    return addresses;
}

FoliageCookedPages Cook(const FoliageCookInput& input, BorrowedView<const FoliagePageAddress> addresses)
{
    FoliageCookedPages cooked;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryCookFoliagePages(input, addresses, cooked, diagnostic) == FoliageCookStatus::Ok);
    return cooked;
}

Array<uint8> Write(const FoliageCookInput& input, const FoliageCookedPages& cooked)
{
    Array<uint8> bytes;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryWriteFoliageCookedPageSet(input, cooked, bytes, diagnostic) == FoliageCookStatus::Ok);
    return bytes;
}

void Merge(const FoliageCookedPages& source, FoliageCookedPages& inoutDestination)
{
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryMergeFoliageCookedPages(source, inoutDestination, diagnostic) == FoliageCookStatus::Ok);
}

bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right)
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

pages::FoliagePageSetView Read(const Array<uint8>& bytes)
{
    pages::FoliagePageSetView view;
    pages::FoliagePageDiagnostic diagnostic;
    const pages::FoliagePageBinding binding
    {
        .DomainId = golden::DOMAIN_ID,
        .TerrainRevision = golden::TERRAIN_REVISION,
    };
    REQUIRE(pages::ReadFoliagePageSet({bytes.GetData(), bytes.GetSize()}, binding, {}, view, diagnostic) ==
            pages::FoliagePageStatus::Ok);
    return view;
}

Array<pages::FoliagePlacement> ReadPlacements(const Array<uint8>& bytes)
{
    const pages::FoliagePageSetView view = Read(bytes);
    Array<pages::FoliagePlacement> placements;
    for (usize i = 0; i < view.GetPlacementCount(); ++i)
    {
        pages::FoliagePlacement placement;
        REQUIRE(view.TryGetPlacement(i, placement) == pages::FoliagePageStatus::Ok);
        placements.Add(placement);
    }
    return placements;
}

uint32 GetCellSize(const golden::GoldenCook& cook, uint16 layer)
{
    for (const FoliageCookLayer& entry : cook.Layers)
    {
        if (entry.Id == layer)
        {
            return entry.CellSize;
        }
    }
    FAIL("unknown layer");
    return 1;
}

usize CountLayer(const Array<pages::FoliagePlacement>& placements, uint16 layer)
{
    usize count = 0;
    for (const pages::FoliagePlacement& placement : placements)
    {
        count += placement.Layer == layer ? 1 : 0;
    }
    return count;
}

/// Global terrain sample coordinates of a placement root.
struct RootPosition final
{
    float64 SampleX = 0;
    float64 SampleZ = 0;
    int64 CellX = 0;
    int64 CellZ = 0;
};

RootPosition GetRootPosition(const golden::GoldenCook& cook, const pages::FoliagePlacement& placement)
{
    const pages::FoliageRoot root = pages::ComputeFoliageRoot(GetCellSize(cook, placement.Layer), placement);
    RootPosition position;
    position.SampleX = static_cast<float64>(cook.Domain.OriginX) + static_cast<float64>(root.X) / LATTICE_UNITS;
    position.SampleZ = static_cast<float64>(cook.Domain.OriginZ) + static_cast<float64>(root.Z) / LATTICE_UNITS;
    const int64 floorX = root.X >= 0 ? root.X / LATTICE_UNITS : -((-root.X + LATTICE_UNITS - 1) / LATTICE_UNITS);
    const int64 floorZ = root.Z >= 0 ? root.Z / LATTICE_UNITS : -((-root.Z + LATTICE_UNITS - 1) / LATTICE_UNITS);
    position.CellX = cook.Domain.OriginX + floorX;
    position.CellZ = cook.Domain.OriginZ + floorZ;
    return position;
}

/// The binary64 reference surface at a placement root.
terrain::TerrainSurfaceSample SampleReference(const golden::GoldenCook& cook, const pages::FoliagePlacement& placement)
{
    const RootPosition position = GetRootPosition(cook, placement);
    const int64 localX = position.CellX - golden::MINIMUM_SAMPLE;
    const int64 localZ = position.CellZ - golden::MINIMUM_SAMPLE;
    const int64 tileColumn = localX / golden::CELLS_PER_SIDE;
    const int64 tileRow = localZ / golden::CELLS_PER_SIDE;
    const terrain::TerrainTile& tile = cook.Tiles[static_cast<usize>(tileRow * golden::TILES_PER_AXIS + tileColumn)];
    const float64 originX = static_cast<float64>(tile.GetDescription().Address.X * golden::CELLS_PER_SIDE);
    const float64 originZ = static_cast<float64>(tile.GetDescription().Address.Z * golden::CELLS_PER_SIDE);
    terrain::TerrainSurfaceSample sample;
    REQUIRE(terrain::TrySampleTerrainTile(tile, position.SampleX - originX, position.SampleZ - originZ, sample) ==
            terrain::TerrainTileStatus::Success);
    return sample;
}

/// Whether two placements violate the exclusion rule.
bool AreConflicting(const golden::GoldenCook& cook,
                    const pages::FoliagePlacement& left,
                    const pages::FoliagePlacement& right)
{
    const pages::FoliageRoot first = pages::ComputeFoliageRoot(GetCellSize(cook, left.Layer), left);
    const pages::FoliageRoot second = pages::ComputeFoliageRoot(GetCellSize(cook, right.Layer), right);
    const int64 differenceX = first.X - second.X;
    const int64 differenceZ = first.Z - second.Z;
    const int64 distanceSquared = differenceX * differenceX + differenceZ * differenceZ;
    for (usize i = 0; i < FOLIAGE_EXCLUSION_CLASS_COUNT; ++i)
    {
        const uint64 leftRadius = cook.Species[left.Species].ExclusionRadii[i];
        const uint64 rightRadius = cook.Species[right.Species].ExclusionRadii[i];
        if (leftRadius == 0 || rightRadius == 0)
        {
            continue;
        }
        const int64 reach = static_cast<int64>((leftRadius * left.Scale + format::SCALE_UNITS_PER_ONE - 1) /
                                               format::SCALE_UNITS_PER_ONE) +
                            static_cast<int64>((rightRadius * right.Scale + format::SCALE_UNITS_PER_ONE - 1) /
                                               format::SCALE_UNITS_PER_ONE);
        if (distanceSquared < reach * reach)
        {
            return true;
        }
    }
    return false;
}

FoliageCandidateKey GetKey(const pages::FoliagePlacement& placement)
{
    return { .Layer = placement.Layer, .Slot = placement.Slot, .CellX = placement.CellX, .CellZ = placement.CellZ };
}

bool ContainsKey(const Array<pages::FoliagePlacement>& placements, const FoliageCandidateKey& key)
{
    for (const pages::FoliagePlacement& placement : placements)
    {
        if (GetKey(placement) == key)
        {
            return true;
        }
    }
    return false;
}

/// Pages cooked by one worker thread.
struct CookJob final
{
    FoliageCookInput Input;
    Array<FoliagePageAddress> Pages;
    FoliageCookedPages Result;
    FoliageCookStatus Status = FoliageCookStatus::InvalidArgument;

    void operator()() noexcept
    {
        FoliageCookDiagnostic diagnostic;
        Status = TryCookFoliagePages(Input, {Pages.GetData(), Pages.GetSize()}, Result, diagnostic);
    }
};
} // namespace

TEST_CASE("foliage random addresses are injective over the full key range", "[foliage][cook]")
{
    constexpr int32 INT32_FIRST = -2147483647 - 1;
    constexpr int32 INT32_LAST = 2147483647;
    const FoliageCandidateKey keys[] = {
        { .Layer = 0, .Slot = 0, .CellX = 0, .CellZ = 0 },
        { .Layer = 0, .Slot = 0, .CellX = -1, .CellZ = 0 },
        { .Layer = 0, .Slot = 0, .CellX = 0, .CellZ = -1 },
        { .Layer = 0, .Slot = 1, .CellX = 0, .CellZ = 0 },
        { .Layer = 1, .Slot = 0, .CellX = 0, .CellZ = 0 },
        { .Layer = 65535, .Slot = 63, .CellX = INT32_LAST, .CellZ = INT32_FIRST },
        { .Layer = 65535, .Slot = 63, .CellX = INT32_FIRST, .CellZ = INT32_LAST },
        { .Layer = 1, .Slot = 0, .CellX = 65536, .CellZ = 0 },
        { .Layer = 0, .Slot = 1, .CellX = 0, .CellZ = 65536 },
    };
    for (const FoliageCandidateKey& key : keys)
    {
        const ludus::foundation::math::RandomAddress address =
            PackFoliageRandomAddress(key, FOLIAGE_PRIORITY_DIMENSION);
        CHECK(UnpackFoliageRandomAddress(address) == key);
        CHECK(address.Dimension == FOLIAGE_PRIORITY_DIMENSION);
        for (const FoliageCandidateKey& other : keys)
        {
            const ludus::foundation::math::RandomAddress otherAddress =
                PackFoliageRandomAddress(other, FOLIAGE_PRIORITY_DIMENSION);
            const bool isSameAddress = address.Scope == otherAddress.Scope && address.Event == otherAddress.Event;
            CHECK(isSameAddress == (key == other));
        }
    }
    // Cells beyond the room-scoped scatter's int16 packing keep distinct addresses.
    const FoliageCandidateKey wide{ .Layer = 3, .Slot = 2, .CellX = 70000, .CellZ = -70000 };
    const FoliageCandidateKey narrow{ .Layer = 3, .Slot = 2, .CellX = 70000 - 65536, .CellZ = -70000 + 65536 };
    CHECK(PackFoliageRandomAddress(wide, 0).Scope != PackFoliageRandomAddress(narrow, 0).Scope);
}

TEST_CASE("the golden foliage cook matches its pinned bytes", "[foliage][cook]")
{
    Fixture fixture;
    const Array<uint8> bytes = CookAll(fixture.Cook.MakeInput());
    const uint64 digest = pages::golden::ComputeDigest({bytes.GetData(), bytes.GetSize()});
    UNSCOPED_INFO("bytes " << bytes.GetSize() << " digest " << digest);
    CHECK(bytes.GetSize() == golden::ENCODED_BYTES);
    CHECK(digest == golden::ENCODED_DIGEST);
    const pages::FoliagePageSetView view = Read(bytes);
    CHECK(view.GetLayerCount() == 3);
    CHECK(view.GetSpeciesCount() == 3);
    CHECK(view.GetHeader().GeneratorVersion == FOLIAGE_COOK_GENERATOR_VERSION);
    const Array<pages::FoliagePlacement> placements = ReadPlacements(bytes);
    CHECK(CountLayer(placements, golden::TREE_LAYER) > 0);
    CHECK(CountLayer(placements, golden::SHRUB_LAYER) > 0);
    CHECK(CountLayer(placements, golden::GRASS_LAYER) > 0);
}

TEST_CASE("full, shuffled, parallel and isolated seam-page cooks produce identical records", "[foliage][cook]")
{
    Fixture fixture;
    const FoliageCookInput input = fixture.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(input);
    REQUIRE(all.GetSize() > WORKER_COUNT);
    bool hasNegativePage = false;
    for (const FoliagePageAddress& page : all)
    {
        hasNegativePage = hasNegativePage == true || (page.X < 0 && page.Z < 0);
    }
    CHECK(hasNegativePage == true);

    const Array<uint8> full = Write(input, Cook(input, {all.GetData(), all.GetSize()}));

    Array<FoliagePageAddress> shuffled;
    for (usize i = 0; i < all.GetSize(); ++i)
    {
        // A fixed permutation: stride 7 over the page count, which is coprime with it.
        shuffled.Add(all[(i * 7) % all.GetSize()]);
    }
    REQUIRE(all.GetSize() % 7 != 0);
    CHECK(AreBytesEqual(Write(input, Cook(input, {shuffled.GetData(), shuffled.GetSize()})), full) == true);

    FoliageCookedPages isolated;
    for (usize i = all.GetSize(); i > 0; --i)
    {
        Merge(Cook(input, {&all[i - 1], 1}), isolated);
    }
    CHECK(AreBytesEqual(Write(input, isolated), full) == true);

    ludus::foundation::StaticArray<CookJob, WORKER_COUNT> jobs{};
    ludus::foundation::StaticArray<threading::Thread, WORKER_COUNT> threads{};
    for (usize i = 0; i < all.GetSize(); ++i)
    {
        jobs[i % WORKER_COUNT].Pages.Add(shuffled[i]);
    }
    for (usize i = 0; i < WORKER_COUNT; ++i)
    {
        jobs[i].Input = input;
        if (threads[i].Start(jobs[i]) != threading::ThreadStatus::Ok)
        {
            jobs[i]();
        }
    }
    FoliageCookedPages parallel;
    for (usize i = 0; i < WORKER_COUNT; ++i)
    {
        if (threads[i].IsJoinable() == true)
        {
            REQUIRE(threads[i].Join() == threading::ThreadStatus::Ok);
        }
        REQUIRE(jobs[i].Status == FoliageCookStatus::Ok);
        Merge(jobs[i].Result, parallel);
    }
    CHECK(AreBytesEqual(Write(input, parallel), full) == true);

    // The seam pages around the negative origin, cooked alone, keep the records of the full cook.
    const FoliagePageAddress seam[] = {
        { .X = -1, .Z = -1 },
        { .X = 0, .Z = -1 },
        { .X = -1, .Z = 0 },
        { .X = 0, .Z = 0 },
    };
    const pages::FoliagePageSetView fullView = Read(full);
    for (const FoliagePageAddress& page : seam)
    {
        const FoliageCookedPages alone = Cook(input, {&page, 1});
        REQUIRE(alone.GetPages().GetSize() == 1);
        usize index = 0;
        REQUIRE(fullView.TryFindPage(page.X, page.Z, index) == pages::FoliagePageStatus::Ok);
        pages::FoliagePage fullPage;
        REQUIRE(fullView.TryGetPage(index, fullPage) == pages::FoliagePageStatus::Ok);
        const pages::FoliagePage& alonePage = alone.GetPages()[0];
        REQUIRE(alonePage.PlacementCount == fullPage.PlacementCount);
        CHECK(alonePage.Bounds == fullPage.Bounds);
        for (uint32 i = 0; i < fullPage.PlacementCount; ++i)
        {
            pages::FoliagePlacement placement;
            REQUIRE(fullView.TryGetPlacement(fullPage.FirstPlacement + i, placement) == pages::FoliagePageStatus::Ok);
            CHECK(alone.GetPlacements()[alonePage.FirstPlacement + i] == placement);
        }
    }

    // Placements straddle the negative seam in every layer.
    const Array<pages::FoliagePlacement> placements = ReadPlacements(full);
    bool hasNegativeCell = false;
    bool hasPositiveCell = false;
    for (const pages::FoliagePlacement& placement : placements)
    {
        hasNegativeCell = hasNegativeCell == true || (placement.CellX < 0 && placement.CellZ < 0);
        hasPositiveCell = hasPositiveCell == true || (placement.CellX >= 0 && placement.CellZ >= 0);
    }
    CHECK(hasNegativeCell == true);
    CHECK(hasPositiveCell == true);
}

TEST_CASE("cooked heights and up directions follow the canonical surface query", "[foliage][cook]")
{
    Fixture fixture;
    const Array<pages::FoliagePlacement> placements = ReadPlacements(CookAll(fixture.Cook.MakeInput()));
    REQUIRE(placements.IsEmpty() == false);
    usize alignedCount = 0;
    for (const pages::FoliagePlacement& placement : placements)
    {
        const terrain::TerrainSurfaceSample reference = SampleReference(fixture.Cook, placement);
        const float64 heightUnits = reference.HeightMetres * format::ROOT_HEIGHT_UNITS_PER_METRE;
        // Exact interpolation rounds once; binary64 evaluation differs from it by far less than one unit.
        CHECK(static_cast<float64>(placement.Height) - heightUnits <= 0.5 + 1e-6);
        CHECK(heightUnits - static_cast<float64>(placement.Height) <= 0.5 + 1e-6);
        if (fixture.Cook.Species[placement.Species].IsAlignedToTerrain == true)
        {
            ++alignedCount;
            const float64 normalX = reference.NormalTerrain.X * format::NORMAL_UNITS;
            const float64 normalZ = reference.NormalTerrain.Z * format::NORMAL_UNITS;
            CHECK(static_cast<float64>(placement.NormalX) - normalX <= 1.0 + 1e-6);
            CHECK(normalX - static_cast<float64>(placement.NormalX) <= 1.0 + 1e-6);
            CHECK(static_cast<float64>(placement.NormalZ) - normalZ <= 1.0 + 1e-6);
            CHECK(normalZ - static_cast<float64>(placement.NormalZ) <= 1.0 + 1e-6);
        }
        else
        {
            CHECK(placement.NormalX == 0);
            CHECK(placement.NormalZ == 0);
        }
    }
    CHECK(alignedCount > 0);
}

TEST_CASE("holes, slope, wetness and masks limit where foliage roots", "[foliage][cook]")
{
    Fixture fixture;
    const Array<pages::FoliagePlacement> placements = ReadPlacements(CookAll(fixture.Cook.MakeInput()));
    usize ridgeTrees = 0;
    for (const pages::FoliagePlacement& placement : placements)
    {
        const RootPosition position = GetRootPosition(fixture.Cook, placement);
        CHECK(golden::IsHoleCell(position.CellX, position.CellZ) == false);
        if (placement.Layer == golden::SHRUB_LAYER)
        {
            // Slope one with a half-unit feather: no shrub on the slope-three ridge, none where the mask is zero.
            const terrain::TerrainSurfaceSample reference = SampleReference(fixture.Cook, placement);
            const float64 horizontal = reference.NormalTerrain.X * reference.NormalTerrain.X +
                                       reference.NormalTerrain.Z * reference.NormalTerrain.Z;
            CHECK(horizontal < 1.5 * 1.5 * reference.NormalTerrain.Y * reference.NormalTerrain.Y);
            CHECK(position.SampleX > static_cast<float64>(golden::MASK_ZERO_COLUMN));
        }
        if (placement.Layer == golden::GRASS_LAYER)
        {
            // Wetness rises by 2048 per row from the minimum row; below 8192 the feather reaches zero.
            CHECK(position.SampleZ > static_cast<float64>(golden::MINIMUM_SAMPLE + 4));
        }
        if (placement.Layer == golden::TREE_LAYER && position.CellX > golden::RIDGE_START)
        {
            ++ridgeTrees;
        }
    }
    CHECK(ridgeTrees > 0);

    // Without the hole channel, candidates in the former hole are admitted.
    fixture.Cook.HasHoles = false;
    const Array<pages::FoliagePlacement> solid = ReadPlacements(CookAll(fixture.Cook.MakeInput()));
    usize holeRoots = 0;
    for (const pages::FoliagePlacement& placement : solid)
    {
        const RootPosition position = GetRootPosition(fixture.Cook, placement);
        holeRoots += golden::IsHoleCell(position.CellX, position.CellZ) == true ? 1 : 0;
    }
    CHECK(holeRoots > 0);
}

TEST_CASE("missing channels are neutral when optional and named when required", "[foliage][cook]")
{
    Fixture fixture;
    // An absent optional mask is neutral: the same records as a mask of ones.
    for (uint16& value : fixture.Cook.MaskValues)
    {
        value = static_cast<uint16>(FOLIAGE_FIELD_UNITS_ONE);
    }
    const Array<uint8> ones = CookAll(fixture.Cook.MakeInput());
    fixture.Cook.MaskCount = 0;
    const FoliageCookInput withoutMask = fixture.Cook.MakeInput();
    const Array<pages::FoliagePlacement> neutral = ReadPlacements(CookAll(withoutMask));
    const Array<pages::FoliagePlacement> masked = ReadPlacements(ones);
    REQUIRE(neutral.GetSize() == masked.GetSize());
    for (usize i = 0; i < neutral.GetSize(); ++i)
    {
        CHECK(neutral[i] == masked[i]);
    }

    // A required mask that the terrain lacks names the layer and the mask.
    fixture.Cook.ShrubMasks[0].IsRequired = true;
    FoliageCookDiagnostic diagnostic;
    CHECK(ValidateFoliageCookInput(fixture.Cook.MakeInput(), diagnostic) == FoliageCookStatus::MissingField);
    CHECK(diagnostic.Issue == FoliageCookIssue::MissingMask);
    CHECK(diagnostic.Index == 1);
    CHECK(diagnostic.FieldId == golden::MASK_ID);

    // A zero mask removes every shrub.
    fixture.Cook.MaskCount = 1;
    for (uint16& value : fixture.Cook.MaskValues)
    {
        value = 0;
    }
    CHECK(CountLayer(ReadPlacements(CookAll(fixture.Cook.MakeInput())), golden::SHRUB_LAYER) == 0);

    // Required wetness is named likewise.
    fixture.Cook.HasWetness = false;
    CHECK(ValidateFoliageCookInput(fixture.Cook.MakeInput(), diagnostic) == FoliageCookStatus::MissingField);
    CHECK(diagnostic.Issue == FoliageCookIssue::MissingWetness);
    CHECK(diagnostic.Index == 2);
}

TEST_CASE("mixed-species strata keep their exclusion classes", "[foliage][cook]")
{
    Fixture fixture;
    const Array<pages::FoliagePlacement> placements = ReadPlacements(CookAll(fixture.Cook.MakeInput()));
    usize conflicts = 0;
    for (usize i = 0; i < placements.GetSize(); ++i)
    {
        for (usize j = i + 1; j < placements.GetSize(); ++j)
        {
            conflicts += AreConflicting(fixture.Cook, placements[i], placements[j]) == true ? 1 : 0;
        }
    }
    CHECK(conflicts == 0);

    // Shrubs share the canopy class with trees, so none grows under a canopy; grass shares only the trunk class,
    // so some grass grows under canopies.
    bool hasGrassUnderCanopy = false;
    for (const pages::FoliagePlacement& tree : placements)
    {
        if (tree.Layer != golden::TREE_LAYER)
        {
            continue;
        }
        const pages::FoliageRoot treeRoot = pages::ComputeFoliageRoot(GetCellSize(fixture.Cook, tree.Layer), tree);
        const int64 canopy = fixture.Cook.Species[golden::TREE_SPECIES].ExclusionRadii[golden::CANOPY_CLASS];
        for (const pages::FoliagePlacement& other : placements)
        {
            if (other.Layer != golden::GRASS_LAYER || other.Species != golden::GRASS_SPECIES)
            {
                continue;
            }
            const pages::FoliageRoot root = pages::ComputeFoliageRoot(GetCellSize(fixture.Cook, other.Layer), other);
            const int64 differenceX = root.X - treeRoot.X;
            const int64 differenceZ = root.Z - treeRoot.Z;
            hasGrassUnderCanopy =
                hasGrassUnderCanopy == true || differenceX * differenceX + differenceZ * differenceZ < canopy * canopy;
        }
    }
    CHECK(hasGrassUnderCanopy == true);

    // Removing the tree stratum frees space that trunks and canopies occupied.
    const usize grassWithTrees = CountLayer(placements, golden::GRASS_LAYER);
    const usize shrubsWithTrees = CountLayer(placements, golden::SHRUB_LAYER);
    FoliageCookInput withoutTrees = fixture.Cook.MakeInput();
    withoutTrees.Layers = {fixture.Cook.Layers.GetData() + 1, 2};
    const Array<pages::FoliagePlacement> understory = ReadPlacements(CookAll(withoutTrees));
    CHECK(CountLayer(understory, golden::SHRUB_LAYER) > shrubsWithTrees);
    CHECK(CountLayer(understory, golden::GRASS_LAYER) != grassWithTrees);
}

TEST_CASE("suppressions stay with their keys while the candidate is absent", "[foliage][cook]")
{
    Fixture fixture;
    fixture.Cook.SuppressionCount = 0;
    const Array<pages::FoliagePlacement> base = ReadPlacements(CookAll(fixture.Cook.MakeInput()));
    FoliageCandidateKey target;
    bool hasTarget = false;
    for (const pages::FoliagePlacement& placement : base)
    {
        if (placement.Layer == golden::TREE_LAYER && hasTarget == false)
        {
            target = GetKey(placement);
            hasTarget = true;
        }
    }
    REQUIRE(hasTarget == true);

    const auto cookPages = [&fixture]() {
        const FoliageCookInput input = fixture.Cook.MakeInput();
        const Array<FoliagePageAddress> all = GetTerrainPages(input);
        return Cook(input, {all.GetData(), all.GetSize()});
    };
    fixture.Cook.Suppressions[0] = target;
    fixture.Cook.SuppressionCount = 1;
    FoliageCookedPages suppressed = cookPages();
    CHECK(suppressed.GetActiveSuppressionCount() == 1);
    CHECK(ContainsKey(ReadPlacements(Write(fixture.Cook.MakeInput(), suppressed)), target) == false);

    // With no tree density the candidate is absent and its suppression is dormant, not removed.
    const uint32 density = fixture.Cook.Layers[0].Density;
    fixture.Cook.Layers[0].Density = 0;
    const FoliageCookedPages dormant = cookPages();
    CHECK(dormant.GetActiveSuppressionCount() == 0);
    CHECK(CountLayer(ReadPlacements(Write(fixture.Cook.MakeInput(), dormant)), golden::TREE_LAYER) == 0);

    // Restoring the density brings the candidate back, still suppressed.
    fixture.Cook.Layers[0].Density = density;
    suppressed = cookPages();
    CHECK(suppressed.GetActiveSuppressionCount() == 1);
    CHECK(ContainsKey(ReadPlacements(Write(fixture.Cook.MakeInput(), suppressed)), target) == false);

    // Removing the suppression restores the placement.
    fixture.Cook.SuppressionCount = 0;
    CHECK(ContainsKey(ReadPlacements(CookAll(fixture.Cook.MakeInput())), target) == true);

    // Unordered suppressions are rejected before cooking.
    fixture.Cook.Suppressions[0] = target;
    fixture.Cook.Suppressions[1] = target;
    fixture.Cook.SuppressionCount = 2;
    FoliageCookDiagnostic diagnostic;
    CHECK(ValidateFoliageCookInput(fixture.Cook.MakeInput(), diagnostic) == FoliageCookStatus::InvalidInput);
    CHECK(diagnostic.Issue == FoliageCookIssue::SuppressionOrder);
    CHECK(diagnostic.Index == 1);
}

TEST_CASE("dirty-halo recooks equal a clean cook", "[foliage][cook]")
{
    Fixture before;
    const FoliageCookInput beforeInput = before.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(beforeInput);
    const FoliageCookedPages previous = Cook(beforeInput, {all.GetData(), all.GetSize()});
    const Array<uint8> previousBytes = Write(beforeInput, previous);

    // Raise a small patch of samples at the terrain's negative corner. The halo reaches about 12 samples, so
    // pages on the far side stay clean.
    constexpr int64 PATCH_FIRST = golden::MINIMUM_SAMPLE;
    constexpr int64 PATCH_LAST = golden::MINIMUM_SAMPLE + 1;
    constexpr int64 PATCH_CODES = 300;
    golden::GoldenCook after;
    REQUIRE(golden::TryInitializeGoldenCook(after, [](int64 x, int64 z) noexcept {
                const bool isInPatch = x >= PATCH_FIRST && x <= PATCH_LAST && z >= PATCH_FIRST && z <= PATCH_LAST;
                return golden::ComputeCode(x, z) + (isInPatch == true ? PATCH_CODES : 0);
            }) == true);
    const FoliageCookInput afterInput = after.MakeInput();
    const Array<uint8> clean = CookAll(afterInput);
    CHECK(AreBytesEqual(clean, previousBytes) == false);

    FoliageCookDiagnostic diagnostic;
    FoliageLatticeRectangle roots;
    REQUIRE(TryGetFoliageSampleEditRoots(afterInput,
                                         { .X = PATCH_FIRST, .Z = PATCH_FIRST },
                                         { .X = PATCH_LAST, .Z = PATCH_LAST },
                                         roots,
                                         diagnostic) == FoliageCookStatus::Ok);
    Array<FoliagePageAddress> dirty;
    REQUIRE(TryGetFoliageDirtyPages(afterInput, roots, dirty, diagnostic) == FoliageCookStatus::Ok);
    CHECK(dirty.IsEmpty() == false);
    CHECK(dirty.GetSize() < all.GetSize());
    FoliageCookedPages spliced = Cook(beforeInput, {all.GetData(), all.GetSize()});
    Merge(Cook(afterInput, {dirty.GetData(), dirty.GetSize()}), spliced);
    CHECK(AreBytesEqual(Write(afterInput, spliced), clean) == true);

    // A suppression edit dirties the pages its candidate's thinning and occupancy reach.
    const Array<pages::FoliagePlacement> placements = ReadPlacements(clean);
    REQUIRE(placements.IsEmpty() == false);
    const FoliageCandidateKey target = GetKey(placements[placements.GetSize() / 2]);
    // Keep the fixture's existing suppression and insert the new key in identity order.
    const FoliageCandidateKey existing = after.Suppressions[0];
    REQUIRE(after.SuppressionCount == 1);
    REQUIRE((existing == target) == false);
    after.Suppressions[0] = IsFoliageKeyBefore(existing, target) == true ? existing : target;
    after.Suppressions[1] = IsFoliageKeyBefore(existing, target) == true ? target : existing;
    after.SuppressionCount = 2;
    const FoliageCookInput suppressedInput = after.MakeInput();
    REQUIRE(TryGetFoliageCandidateRoots(suppressedInput, target, roots, diagnostic) == FoliageCookStatus::Ok);
    REQUIRE(TryGetFoliageDirtyPages(suppressedInput, roots, dirty, diagnostic) == FoliageCookStatus::Ok);
    Merge(Cook(suppressedInput, {dirty.GetData(), dirty.GetSize()}), spliced);
    CHECK(AreBytesEqual(Write(suppressedInput, spliced), CookAll(suppressedInput)) == true);
}

TEST_CASE("a dirty recook replaces the published page set only after it validates", "[foliage][cook]")
{
    const pages::FoliagePageBinding binding
    {
        .DomainId = golden::DOMAIN_ID,
        .TerrainRevision = golden::TERRAIN_REVISION,
    };
    Fixture before;
    const FoliageCookInput beforeInput = before.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(beforeInput);
    const FoliageCookedPages previous = Cook(beforeInput, {all.GetData(), all.GetSize()});
    const Array<uint8> previousBytes = Write(beforeInput, previous);

    pages::FoliagePageSetPublication publication;
    pages::FoliagePageSetCandidate candidate;
    pages::FoliagePageDiagnostic diagnostic;
    Array<uint8> staged = previousBytes;
    REQUIRE(publication.TryStage(staged, binding, candidate, diagnostic) == pages::FoliagePageStatus::Ok);
    REQUIRE(publication.TryPublish(candidate) == pages::FoliagePublicationStatus::Ok);

    // Recook the pages a terrain edit dirties and splice them over the published cook.
    constexpr int64 PATCH_CODES = 300;
    golden::GoldenCook after;
    REQUIRE(golden::TryInitializeGoldenCook(after, [](int64 x, int64 z) noexcept {
                const bool isInPatch = x <= golden::MINIMUM_SAMPLE + 1 && z <= golden::MINIMUM_SAMPLE + 1;
                return golden::ComputeCode(x, z) + (isInPatch == true ? PATCH_CODES : 0);
            }) == true);
    const FoliageCookInput afterInput = after.MakeInput();
    FoliageCookDiagnostic cookDiagnostic;
    FoliageLatticeRectangle roots;
    REQUIRE(TryGetFoliageSampleEditRoots(afterInput,
                                         { .X = golden::MINIMUM_SAMPLE, .Z = golden::MINIMUM_SAMPLE },
                                         { .X = golden::MINIMUM_SAMPLE + 1, .Z = golden::MINIMUM_SAMPLE + 1 },
                                         roots,
                                         cookDiagnostic) == FoliageCookStatus::Ok);
    Array<FoliagePageAddress> dirty;
    REQUIRE(TryGetFoliageDirtyPages(afterInput, roots, dirty, cookDiagnostic) == FoliageCookStatus::Ok);
    FoliageCookedPages spliced = Cook(beforeInput, {all.GetData(), all.GetSize()});
    Merge(Cook(afterInput, {dirty.GetData(), dirty.GetSize()}), spliced);
    Array<uint8> replacement = Write(afterInput, spliced);
    const Array<uint8> clean = CookAll(afterInput);
    REQUIRE(AreBytesEqual(replacement, clean) == true);
    REQUIRE(AreBytesEqual(replacement, previousBytes) == false);

    // A replacement damaged after the cook, or bound to another terrain revision, keeps the published cook.
    Array<uint8> damaged = replacement;
    damaged[damaged.GetSize() - 1] ^= 1U;
    CHECK(publication.TryStage(damaged, binding, candidate, diagnostic) != pages::FoliagePageStatus::Ok);
    CHECK(publication.TryPublish(candidate) == pages::FoliagePublicationStatus::NotReady);
    pages::FoliagePageBinding newerTerrain = binding;
    newerTerrain.TerrainRevision += 1;
    CHECK(publication.TryStage(replacement, newerTerrain, candidate, diagnostic) ==
          pages::FoliagePageStatus::RevisionMismatch);
    CHECK(publication.TryPublish(candidate) == pages::FoliagePublicationStatus::NotReady);
    CHECK(publication.GetAcceptedRevision() == 1);
    CHECK(pages::golden::ComputeDigest(publication.GetAcceptedBytes()) == golden::ENCODED_DIGEST);

    // The validated replacement publishes the clean cook's bytes and retires the previous generation.
    REQUIRE(publication.TryStage(replacement, binding, candidate, diagnostic) == pages::FoliagePageStatus::Ok);
    REQUIRE(publication.TryPublish(candidate) == pages::FoliagePublicationStatus::Ok);
    CHECK(publication.GetAcceptedRevision() == 2);
    const BorrowedView<const uint8> accepted = publication.GetAcceptedBytes();
    CHECK(pages::golden::ComputeDigest(accepted) == pages::golden::ComputeDigest({clean.GetData(), clean.GetSize()}));
    CHECK(accepted.GetSize() == clean.GetSize());
    REQUIRE(candidate.GetState() == pages::FoliagePageSetCandidateState::Retired);
    CHECK(candidate.GetView().GetPlacementCount() == Read(previousBytes).GetPlacementCount());
}

TEST_CASE("render-only species edits keep the page set bytes", "[foliage][cook]")
{
    Fixture fixture;
    const Array<uint8> original = CookAll(fixture.Cook.MakeInput());
    fixture.Cook.Species[golden::TREE_SPECIES].RenderRevision += 1;
    CHECK(AreBytesEqual(CookAll(fixture.Cook.MakeInput()), original) == true);

    // A placement revision is part of the cache key but leaves the records alone.
    fixture.Cook.Species[golden::TREE_SPECIES].PlacementRevision += 1;
    const Array<uint8> revised = CookAll(fixture.Cook.MakeInput());
    CHECK(AreBytesEqual(revised, original) == false);
    const Array<pages::FoliagePlacement> before = ReadPlacements(original);
    const Array<pages::FoliagePlacement> after = ReadPlacements(revised);
    REQUIRE(before.GetSize() == after.GetSize());
    for (usize i = 0; i < before.GetSize(); ++i)
    {
        CHECK(before[i] == after[i]);
    }
}

TEST_CASE("lattice coordinates are checked at the signed cell limits", "[foliage][cook]")
{
    // One four-cell tile, read by a layer of 65,536 lattice units (256 samples) per cell.
    constexpr uint32 SMALL_CELLS = 4;
    constexpr uint32 WIDE_CELL = format::MAX_CELL_SIZE;
    constexpr int64 LOW_ORIGIN = int64{1} << 39;
    terrain::TerrainTile tiles[1];
    terrain::TerrainTileDescription description;
    description.Revision = golden::TERRAIN_REVISION;
    description.CellsPerSide = SMALL_CELLS;
    constexpr usize CODE_COUNT = (usize{SMALL_CELLS} + 1) * (usize{SMALL_CELLS} + 1);
    const uint8 codes[CODE_COUNT * 2] = {};
    REQUIRE(terrain::TryLoadTerrainTile(description, codes, tiles[0]) == terrain::TerrainTileStatus::Success);

    const FoliagePaletteEntry palette[] = {{ .Species = 0, .Weight = 1 }};
    FoliageCookLayer layer;
    layer.Id = 1;
    layer.SlotsPerCell = 1;
    layer.CellSize = WIDE_CELL;
    layer.Density = 1;
    layer.Palette = palette;
    const FoliageCookLayer layers[] = {layer};
    FoliageCookSpecies species;
    species.Key = 1;
    const FoliageCookSpecies speciesTable[] = {species};
    FoliageCookInput input;
    input.Domain = { .Id = 1, .OriginX = LOW_ORIGIN, .OriginZ = 0, .PageSize = WIDE_CELL };
    input.Terrain.Grid = { .Tiles = tiles, .MinimumTile = {}, .TileColumns = 1, .TileRows = 1 };
    input.Layers = layers;
    input.Species = speciesTable;

    // The terrain starts at lattice -2^47, whose cell is the smallest int32.
    FoliageCookDiagnostic diagnostic;
    REQUIRE(ValidateFoliageCookInput(input, diagnostic) == FoliageCookStatus::Ok);
    Array<FoliagePageAddress> addresses = GetTerrainPages(input);
    REQUIRE(addresses.GetSize() == 1);
    CHECK(addresses[0].X == -2147483647 - 1);
    FoliageCookedPages cooked = Cook(input, {addresses.GetData(), addresses.GetSize()});
    CHECK(cooked.GetPages().GetSize() == 1);
    FoliageLatticeRectangle roots;
    REQUIRE(TryGetFoliageCandidateRoots(input,
                                        { .Layer = 1, .Slot = 0, .CellX = -2147483647 - 1, .CellZ = 0 },
                                        roots,
                                        diagnostic) == FoliageCookStatus::Ok);
    CHECK(roots.MinimumX == -(int64{1} << 47));

    // One sample further, the first cell no longer fits int32.
    input.Domain.OriginX = LOW_ORIGIN + 1;
    CHECK(ValidateFoliageCookInput(input, diagnostic) == FoliageCookStatus::CoordinateOutOfRange);
    CHECK(diagnostic.Issue == FoliageCookIssue::TerrainCoordinates);

    // The positive limit: the last cell is the largest int32.
    input.Domain.OriginX = SMALL_CELLS - LOW_ORIGIN;
    CHECK(ValidateFoliageCookInput(input, diagnostic) == FoliageCookStatus::Ok);
    input.Domain.OriginX = SMALL_CELLS - LOW_ORIGIN - 1;
    CHECK(ValidateFoliageCookInput(input, diagnostic) == FoliageCookStatus::CoordinateOutOfRange);

    // An origin whose lattice offset overflows int64 is rejected, not wrapped.
    input.Domain.OriginX = -(int64{1} << 62);
    CHECK(ValidateFoliageCookInput(input, diagnostic) == FoliageCookStatus::CoordinateOutOfRange);
}

TEST_CASE("invalid cook inputs are rejected with the rule to repair", "[foliage][cook]")
{
    Fixture fixture;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(ValidateFoliageCookInput(fixture.Cook.MakeInput(), diagnostic) == FoliageCookStatus::Ok);

    const auto expect = [&fixture](FoliageCookStatus status, FoliageCookIssue issue, usize index) {
        FoliageCookDiagnostic result;
        CHECK(ValidateFoliageCookInput(fixture.Cook.MakeInput(), result) == status);
        CHECK(result.Issue == issue);
        CHECK(result.Index == index);
        Array<uint8> bytes;
        bytes.Add(1);
        CHECK(golden::TryCookAll(fixture.Cook.MakeInput(), bytes, result) == status);
        CHECK(bytes.GetSize() == 1);
    };

    // Density above one placement per slot.
    const uint32 density = fixture.Cook.Layers[0].Density;
    fixture.Cook.Layers[0].Density = 1U << 31U;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerDensity, 0);
    fixture.Cook.Layers[0].Density = density;

    fixture.Cook.Domain.Id = 0;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::Domain, 0);
    fixture.Cook.Domain.Id = golden::DOMAIN_ID;

    fixture.Cook.Layers[1].CellSize = 3;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerCellSize, 1);
    fixture.Cook.Layers[1].CellSize = 512;

    fixture.Cook.Layers[2].Id = golden::SHRUB_LAYER;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerOrder, 2);
    fixture.Cook.Layers[2].Id = golden::GRASS_LAYER;

    fixture.Cook.GrassPalette[0].Species = 3;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerPalette, 2);
    fixture.Cook.GrassPalette[0].Species = golden::SHRUB_SPECIES;

    fixture.Cook.Layers[2].Height.Maximum = fixture.Cook.Layers[2].Height.Minimum - 1;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerRange, 2);
    fixture.Cook.Layers[2].Height.Maximum = fixture.Cook.Layers[2].Height.Minimum + 1;

    fixture.Cook.Species[1].Key = fixture.Cook.Species[0].Key;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesKey, 1);
    fixture.Cook.Species[1].Key = 0x2002;

    const uint32 radius = fixture.Cook.Species[2].ExclusionRadii[0];
    fixture.Cook.Species[2].ExclusionRadii[0] = MAX_FOLIAGE_RADIUS + 1;
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesRadius, 2);
    fixture.Cook.Species[2].ExclusionRadii[0] = radius;

    fixture.Cook.Holes.RemoveLast();
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::Holes, 0);
    fixture.Cook.Holes.Add(0);

    // A tile whose shared edge disagrees with its neighbour.
    REQUIRE(golden::TryLoadTile(
                golden::MINIMUM_TILE,
                [](int64 x, int64 z) noexcept { return golden::ComputeCode(x, z) + 1; },
                fixture.Cook.Tiles[0]) == true);
    expect(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainSeam, 0);

    // Requests must name distinct pages.
    REQUIRE(golden::TryLoadTile(
                golden::MINIMUM_TILE,
                [](int64 x, int64 z) noexcept { return golden::ComputeCode(x, z); },
                fixture.Cook.Tiles[0]) == true);
    const FoliagePageAddress repeated[] = {{ .X = 0, .Z = 0 }, { .X = 0, .Z = 0 }};
    FoliageCookedPages cooked;
    CHECK(TryCookFoliagePages(fixture.Cook.MakeInput(), repeated, cooked, diagnostic) ==
          FoliageCookStatus::InvalidArgument);
    CHECK(diagnostic.Issue == FoliageCookIssue::PageRequest);
    CHECK(cooked.GetPages().IsEmpty() == true);
}
