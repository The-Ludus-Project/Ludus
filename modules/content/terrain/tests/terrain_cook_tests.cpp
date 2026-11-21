#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/content/terrain_format/terrain_format.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace terrain = ludus::content::terrain;
namespace format = ludus::content::terrain_format;
namespace math = ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::Fingerprint128;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::int64;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using terrain::TerrainCookStatus;

constexpr uint32 PAGE_CELLS = 16;
constexpr uint32 HALF_PAGE_CELLS = 8;
constexpr uint32 PAGES_X = 3;
constexpr uint32 PAGES_Z = 2;
constexpr usize PAGE_COUNT = static_cast<usize>(PAGES_X) * PAGES_Z;
constexpr int64 DOMAIN_CELLS_X = static_cast<int64>(PAGES_X) * PAGE_CELLS;
constexpr int64 DOMAIN_CELLS_Z = static_cast<int64>(PAGES_Z) * PAGE_CELLS;
constexpr int32 HALO = static_cast<int32>(format::PAGE_HALO_SAMPLES);
constexpr uint32 TERRAIN_DOMAIN = 1;
constexpr uint64 SEED = 11;
constexpr uint64 EDITED_SEED = 12;
constexpr float32 QUANTUM_METRES = 1.0F / 64.0F;
constexpr float32 ORIGIN_METRES = -10;
constexpr float64 MAXIMUM_ELEVATION_METRES = 120;
constexpr float64 FEATURE_SIZE_METRES = 24;
constexpr float64 ROUGHNESS = 0.5;
constexpr float32 RAISE_METRES = 3.5F;
/// Binary32 source heights near 120 m carry rounding up to about 4 micrometres on top of the half quantum.
constexpr float64 DECODE_TOLERANCE_METRES = (0.5 * static_cast<float64>(QUANTUM_METRES)) + 1.0e-5;
constexpr usize VERSION_OFFSET = format::MAGIC_BYTES;
constexpr usize PAGE_COUNT_OFFSET =
    format::MANIFEST_HEADER_BYTES - format::DIGEST_BYTES - sizeof(uint64) - sizeof(uint32);
constexpr usize RECORD_OFFSET_FIELD = 2 * sizeof(int64) + sizeof(uint64);

/// The domain's elevation field together with the layout it is cooked with.
struct Domain final
{
    terrain::TerrainRevisionLayout Layout{};
    terrain::TerrainFieldGrid Grid{};
    Array<float32> Elevations;
};

[[nodiscard]] terrain::TerrainRevisionLayout MakeLayout(uint32 cellsPerPage = PAGE_CELLS)
{
    terrain::TerrainRevisionLayout layout;
    layout.Revision = 1;
    layout.RecipeVersion = terrain::TERRAIN_NOISE_LAYOUT_VERSION;
    layout.SourceDigest = {SEED, 0};
    const uint32 scale = PAGE_CELLS / cellsPerPage;
    layout.FirstPage = {-1 * static_cast<int64>(scale), -2 * static_cast<int64>(scale)};
    layout.PagesX = PAGES_X * scale;
    layout.PagesZ = PAGES_Z * scale;
    layout.CellsPerPage = cellsPerPage;
    layout.SpacingMetres = 1;
    layout.Codec =
    {
        .Encoding = terrain::TerrainHeightEncoding::Unsigned16,
        .OriginMetres = ORIGIN_METRES,
        .QuantumMetres = QUANTUM_METRES,
    };
    return layout;
}

[[nodiscard]] terrain::TerrainSampleAddress GetDomainOrigin(const terrain::TerrainRevisionLayout& layout)
{
    terrain::TerrainSampleAddress origin;
    REQUIRE(terrain::TryGetTerrainTileOrigin(layout.FirstPage, layout.CellsPerPage, origin) ==
            terrain::TerrainTileStatus::Success);
    return origin;
}

/// Sample the addressed noise recipe by global lattice coordinates, so every partition sees one field.
void FillDomain(const terrain::TerrainRevisionLayout& layout, uint64 seed, Domain& outDomain)
{
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(seed, TERRAIN_DOMAIN, key) == math::MathStatus::Success);
    terrain::TerrainNoiseShape shape;
    shape.MaximumElevationMetres = MAXIMUM_ELEVATION_METRES;
    shape.FeatureSizeMetres = FEATURE_SIZE_METRES;
    shape.Roughness = ROUGHNESS;
    outDomain.Layout = layout;
    outDomain.Grid =
    {
        .Columns = (layout.PagesX * layout.CellsPerPage) + 1,
        .Rows = (layout.PagesZ * layout.CellsPerPage) + 1,
    };
    REQUIRE(outDomain.Elevations.TryResize(terrain::GetTerrainSampleCount(outDomain.Grid)));
    const terrain::TerrainSampleAddress origin = GetDomainOrigin(layout);
    for (uint32 row = 0; row < outDomain.Grid.Rows; ++row)
    {
        for (uint32 column = 0; column < outDomain.Grid.Columns; ++column)
        {
            const float64 x = static_cast<float64>(origin.X + column) * static_cast<float64>(layout.SpacingMetres);
            const float64 z = static_cast<float64>(origin.Z + row) * static_cast<float64>(layout.SpacingMetres);
            outDomain.Elevations[(static_cast<usize>(row) * outDomain.Grid.Columns) + column] =
                static_cast<float32>(terrain::ComputeTerrainElevation(shape, key, x, z));
        }
    }
}

void CookDomain(const Domain& domain, terrain::TerrainCookedRevision& outRevision)
{
    REQUIRE(terrain::TryCookTerrainRevision(domain.Layout, domain.Grid, domain.Elevations.AsView(), outRevision) ==
            TerrainCookStatus::Success);
}

[[nodiscard]] Array<uint8> CopyBytes(BorrowedView<const uint8> bytes)
{
    Array<uint8> copy;
    REQUIRE(copy.TryAddRange(bytes));
    return copy;
}

[[nodiscard]] BorrowedView<const uint8> GetPageRange(const terrain::TerrainCookedRevision& revision,
                                                     const terrain::TerrainPageRecord& record)
{
    return {revision.GetPagePackBytes().GetDataOrNull() + record.ByteOffset, record.ByteSize};
}

/// Read every page through its own copied byte range, so no page can see another page's bytes.
void ReadPages(const terrain::TerrainCookedRevision& revision,
               terrain::TerrainManifest& outManifest,
               Array<terrain::TerrainPage>& outPages)
{
    REQUIRE(terrain::TryReadTerrainManifest(revision.GetManifestBytes(), outManifest) == TerrainCookStatus::Success);
    const BorrowedView<const terrain::TerrainPageRecord> records = outManifest.GetPageRecords();
    REQUIRE(outPages.TryResize(records.GetSize()));
    for (usize i = 0; i < records.GetSize(); ++i)
    {
        const Array<uint8> range = CopyBytes(GetPageRange(revision, records[i]));
        REQUIRE(terrain::TryReadTerrainPage(outManifest, i, range.AsView(), outPages[i]) == TerrainCookStatus::Success);
    }
}

/// Decode a global sample from the page that owns it in the half-open partition (the last page owns the far edge).
[[nodiscard]] float64 DecodeOwnedSample(const terrain::TerrainManifest& manifest,
                                        const Array<terrain::TerrainPage>& pages,
                                        terrain::TerrainSampleAddress sample)
{
    const terrain::TerrainRevisionLayout& layout = manifest.GetLayout();
    const terrain::TerrainSampleAddress origin = GetDomainOrigin(layout);
    const int64 x = sample.X;
    const int64 z = sample.Z;
    const int64 cells = layout.CellsPerPage;
    const int64 lastX = (static_cast<int64>(layout.PagesX) * cells);
    const int64 lastZ = (static_cast<int64>(layout.PagesZ) * cells);
    const int64 column = (x - origin.X) < 0 ? 0 : ((x - origin.X) > lastX ? lastX : (x - origin.X));
    const int64 row = (z - origin.Z) < 0 ? 0 : ((z - origin.Z) > lastZ ? lastZ : (z - origin.Z));
    const int64 pageColumn = column / cells < layout.PagesX ? column / cells : layout.PagesX - 1;
    const int64 pageRow = row / cells < layout.PagesZ ? row / cells : layout.PagesZ - 1;
    const usize index = static_cast<usize>((pageRow * layout.PagesX) + pageColumn);
    float64 height = 0;
    REQUIRE(terrain::TryGetTerrainPageHeight(pages[index],
                                             static_cast<int32>(column - (pageColumn * cells)),
                                             static_cast<int32>(row - (pageRow * cells)),
                                             height) == TerrainCookStatus::Success);
    return height;
}

void WriteUint32At(Array<uint8>& inoutBytes, usize offset, uint32 value)
{
    REQUIRE(ludus::foundation::TryWriteLittleEndian(value, {inoutBytes.GetData() + offset, 4}));
}

void WriteUint64At(Array<uint8>& inoutBytes, usize offset, uint64 value)
{
    REQUIRE(ludus::foundation::TryWriteLittleEndian(value, {inoutBytes.GetData() + offset, 8}));
}

/// Recompute the trailing manifest digest after a deliberate field edit, so only the semantic check can fail.
void RewriteManifestDigest(Array<uint8>& inoutManifest)
{
    const usize digested = inoutManifest.GetSize() - format::DIGEST_BYTES;
    const Fingerprint128 digest =
        ludus::foundation::StableFingerprint128(std::span<const uint8>{inoutManifest.GetData(), digested});
    WriteUint64At(inoutManifest, digested, digest.Low);
    WriteUint64At(inoutManifest, digested + sizeof(uint64), digest.High);
}

[[nodiscard]] bool AreBytesEqual(BorrowedView<const uint8> first, BorrowedView<const uint8> second)
{
    if (first.GetSize() != second.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < first.GetSize(); ++i)
    {
        if (first[i] != second[i])
        {
            return false;
        }
    }
    return true;
}

/// Raise an inclusive rectangle of global samples, as a flatten-free stamp would.
void RaiseRegion(Domain& inoutDomain, terrain::TerrainSampleRegion region)
{
    const terrain::TerrainSampleAddress origin = GetDomainOrigin(inoutDomain.Layout);
    for (int64 z = region.Minimum.Z; z <= region.Maximum.Z; ++z)
    {
        for (int64 x = region.Minimum.X; x <= region.Maximum.X; ++x)
        {
            const usize index =
                (static_cast<usize>(z - origin.Z) * inoutDomain.Grid.Columns) + static_cast<usize>(x - origin.X);
            inoutDomain.Elevations[index] += RAISE_METRES;
        }
    }
}
} // namespace

TEST_CASE("Terrain revisions cook independently readable pages that match the canonical tile codec", "[terrain][cook]")
{
    Domain domain;
    FillDomain(MakeLayout(), SEED, domain);
    terrain::TerrainCookedRevision revision;
    CookDomain(domain, revision);
    CHECK(revision.GetManifestBytes().GetSize() == format::GetManifestBytes(static_cast<uint32>(PAGE_COUNT)));

    terrain::TerrainManifest manifest;
    Array<terrain::TerrainPage> pages;
    ReadPages(revision, manifest, pages);
    const terrain::TerrainRevisionLayout& layout = manifest.GetLayout();
    CHECK(layout.Revision == domain.Layout.Revision);
    CHECK(layout.RecipeVersion == domain.Layout.RecipeVersion);
    CHECK(layout.SourceDigest == domain.Layout.SourceDigest);
    CHECK(layout.FirstPage.X == -1);
    CHECK(layout.FirstPage.Z == -2);

    for (usize i = 0; i < pages.GetSize(); ++i)
    {
        const terrain::TerrainPage& page = pages[i];
        const terrain::TerrainPageRecord& record = page.GetRecord();
        CHECK(record.ContentRevision == domain.Layout.Revision);
        CHECK(record.MinimumCode <= record.MaximumCode);
        CHECK(static_cast<float64>(record.MaximumErrorMetres) <= DECODE_TOLERANCE_METRES);
        CHECK(page.GetTile().GetDescription().Revision == domain.Layout.Revision);

        // The core is exactly the tile cooked from the same window of the field.
        const uint32 column = static_cast<uint32>(i % PAGES_X) * PAGE_CELLS;
        const uint32 row = static_cast<uint32>(i / PAGES_X) * PAGE_CELLS;
        const terrain::TerrainTileDescription description = page.GetTile().GetDescription();
        terrain::TerrainTile expected;
        REQUIRE(
            terrain::TryCookTerrainTile(description, domain.Grid, domain.Elevations.AsView(), column, row, expected) ==
            terrain::TerrainTileStatus::Success);
        CHECK(AreBytesEqual(page.GetTile().GetEncodedHeights(), expected.GetEncodedHeights()));
    }

    // Every neighbouring pair, diagonals included, shares identical codes.
    for (usize first = 0; first < pages.GetSize(); ++first)
    {
        for (usize second = first + 1; second < pages.GetSize(); ++second)
        {
            const int64 columnGap = static_cast<int64>(second % PAGES_X) - static_cast<int64>(first % PAGES_X);
            const int64 rowGap = static_cast<int64>(second / PAGES_X) - static_cast<int64>(first / PAGES_X);
            if (columnGap >= -1 && columnGap <= 1 && rowGap <= 1)
            {
                CHECK(terrain::CheckTerrainTileSeam(pages[first].GetTile(), pages[second].GetTile()) ==
                      terrain::TerrainTileStatus::Success);
            }
        }
    }

    // A repeated cook is byte-identical.
    terrain::TerrainCookedRevision repeated;
    CookDomain(domain, repeated);
    CHECK(AreBytesEqual(repeated.GetManifestBytes(), revision.GetManifestBytes()));
    CHECK(AreBytesEqual(repeated.GetPagePackBytes(), revision.GetPagePackBytes()));
}

TEST_CASE("Terrain page halos repeat their owners' samples and clamp at the domain boundary", "[terrain][cook]")
{
    Domain domain;
    FillDomain(MakeLayout(), SEED, domain);
    terrain::TerrainCookedRevision revision;
    CookDomain(domain, revision);
    terrain::TerrainManifest manifest;
    Array<terrain::TerrainPage> pages;
    ReadPages(revision, manifest, pages);

    const terrain::TerrainSampleAddress origin = GetDomainOrigin(domain.Layout);
    const int32 cells = static_cast<int32>(PAGE_CELLS);
    usize clampedSamples = 0;
    for (usize i = 0; i < pages.GetSize(); ++i)
    {
        const int64 pageX = origin.X + (static_cast<int64>(i % PAGES_X) * cells);
        const int64 pageZ = origin.Z + (static_cast<int64>(i / PAGES_X) * cells);
        for (int32 row = -HALO; row <= cells + HALO; ++row)
        {
            for (int32 column = -HALO; column <= cells + HALO; ++column)
            {
                float64 height = 0;
                REQUIRE(terrain::TryGetTerrainPageHeight(pages[i], column, row, height) == TerrainCookStatus::Success);
                const int64 x = pageX + column;
                const int64 z = pageZ + row;
                CHECK(height == DecodeOwnedSample(manifest, pages, {x, z}));
                const bool isOutside =
                    x < origin.X || z < origin.Z || x > origin.X + DOMAIN_CELLS_X || z > origin.Z + DOMAIN_CELLS_Z;
                clampedSamples += isOutside == true ? 1 : 0;
            }
        }
    }
    CHECK(clampedSamples > 0);

    float64 height = 0;
    CHECK(terrain::TryGetTerrainPageHeight(pages[0], -HALO - 1, 0, height) == TerrainCookStatus::InvalidArgument);
    CHECK(terrain::TryGetTerrainPageHeight(pages[0], 0, cells + HALO + 1, height) ==
          TerrainCookStatus::InvalidArgument);
    CHECK(height == 0);
    const terrain::TerrainPage empty;
    CHECK(terrain::TryGetTerrainPageHeight(empty, 0, 0, height) == TerrainCookStatus::InvalidStorage);
}

TEST_CASE("Terrain cooking is independent of page partition and cook order", "[terrain][cook]")
{
    Domain coarse;
    FillDomain(MakeLayout(PAGE_CELLS), SEED, coarse);
    Domain fine;
    FillDomain(MakeLayout(HALF_PAGE_CELLS), SEED, fine);
    terrain::TerrainCookedRevision coarseRevision;
    terrain::TerrainCookedRevision fineRevision;
    CookDomain(coarse, coarseRevision);
    CookDomain(fine, fineRevision);
    terrain::TerrainManifest coarseManifest;
    terrain::TerrainManifest fineManifest;
    Array<terrain::TerrainPage> coarsePages;
    Array<terrain::TerrainPage> finePages;
    ReadPages(coarseRevision, coarseManifest, coarsePages);
    ReadPages(fineRevision, fineManifest, finePages);
    REQUIRE(finePages.GetSize() == 4 * coarsePages.GetSize());

    const terrain::TerrainSampleAddress origin = GetDomainOrigin(coarse.Layout);
    for (int64 z = origin.Z; z <= origin.Z + DOMAIN_CELLS_Z; ++z)
    {
        for (int64 x = origin.X; x <= origin.X + DOMAIN_CELLS_X; ++x)
        {
            CHECK(DecodeOwnedSample(coarseManifest, coarsePages, {x, z}) ==
                  DecodeOwnedSample(fineManifest, finePages, {x, z}));
        }
    }

    // Cook the fine pages in a scrambled order, as concurrent workers would finish them, and assemble.
    const usize pageCount = finePages.GetSize();
    constexpr usize STRIDE = 7; // Coprime with the 24 fine pages, so every page is visited once.
    Array<terrain::TerrainCookedPage> scrambled;
    REQUIRE(scrambled.TryResize(pageCount));
    for (usize i = 0; i < pageCount; ++i)
    {
        const usize index = (i * STRIDE) % pageCount;
        const terrain::TerrainTileAddress address = fineManifest.GetPageRecords()[index].Address;
        REQUIRE(terrain::TryCookTerrainPage(fine.Layout, fine.Grid, fine.Elevations.AsView(), address, scrambled[i]) ==
                TerrainCookStatus::Success);
    }
    terrain::TerrainCookedRevision assembled;
    REQUIRE(terrain::TryAssembleTerrainRevision(fine.Layout, scrambled.AsView(), assembled) ==
            TerrainCookStatus::Success);
    CHECK(AreBytesEqual(assembled.GetManifestBytes(), fineRevision.GetManifestBytes()));
    CHECK(AreBytesEqual(assembled.GetPagePackBytes(), fineRevision.GetPagePackBytes()));

    // A duplicated page is rejected without replacing the output.
    terrain::TerrainCookedPage duplicate;
    REQUIRE(terrain::TryCookTerrainPage(fine.Layout,
                                        fine.Grid,
                                        fine.Elevations.AsView(),
                                        scrambled[1].GetRecord().Address,
                                        duplicate) == TerrainCookStatus::Success);
    scrambled[0] = ludus::foundation::Move(duplicate);
    CHECK(terrain::TryAssembleTerrainRevision(fine.Layout, scrambled.AsView(), assembled) ==
          TerrainCookStatus::InvalidArgument);
    CHECK(AreBytesEqual(assembled.GetManifestBytes(), fineRevision.GetManifestBytes()));
}

TEST_CASE("Terrain assembly rejects pages whose shared samples or halos disagree", "[terrain][cook]")
{
    Domain domain;
    FillDomain(MakeLayout(), SEED, domain);
    const terrain::TerrainSampleAddress origin = GetDomainOrigin(domain.Layout);
    const int64 borderX = origin.X + PAGE_CELLS;

    const auto assembleWithEditedFirstPage = [&](terrain::TerrainSampleRegion region) -> TerrainCookStatus {
        Domain changed;
        FillDomain(MakeLayout(), SEED, changed);
        RaiseRegion(changed, region);
        Array<terrain::TerrainCookedPage> pages;
        REQUIRE(pages.TryResize(PAGE_COUNT));
        for (usize i = 0; i < pages.GetSize(); ++i)
        {
            const Domain& source = i == 0 ? changed : domain;
            const terrain::TerrainTileAddress address{domain.Layout.FirstPage.X + static_cast<int64>(i % PAGES_X),
                                                      domain.Layout.FirstPage.Z + static_cast<int64>(i / PAGES_X)};
            REQUIRE(terrain::TryCookTerrainPage(source.Layout,
                                                source.Grid,
                                                source.Elevations.AsView(),
                                                address,
                                                pages[i]) == TerrainCookStatus::Success);
        }
        terrain::TerrainCookedRevision revision;
        return terrain::TryAssembleTerrainRevision(domain.Layout, pages.AsView(), revision);
    };

    // Changing the first page's interior, away from its border and its neighbours' halos, stays consistent.
    const int64 interior = origin.X + (PAGE_CELLS / 2);
    CHECK(assembleWithEditedFirstPage({{interior, origin.Z + 2}, {interior, origin.Z + 2}}) ==
          TerrainCookStatus::Success);
    // A shared edge sample, and a sample only the neighbour's halo repeats, both disagree.
    CHECK(assembleWithEditedFirstPage({{borderX, origin.Z + 2}, {borderX, origin.Z + 2}}) ==
          TerrainCookStatus::BorderMismatch);
    CHECK(assembleWithEditedFirstPage({{borderX - 1, origin.Z + 2}, {borderX - 1, origin.Z + 2}}) ==
          TerrainCookStatus::BorderMismatch);
    CHECK(assembleWithEditedFirstPage({{borderX + 1, origin.Z + 2}, {borderX + 1, origin.Z + 2}}) ==
          TerrainCookStatus::BorderMismatch);
}

TEST_CASE("Terrain edits recook exactly the pages they reach and match a clean cook", "[terrain][cook][edit]")
{
    Domain base;
    FillDomain(MakeLayout(), SEED, base);
    terrain::TerrainCookedRevision baseRevision;
    CookDomain(base, baseRevision);
    terrain::TerrainManifest baseManifest;
    REQUIRE(terrain::TryReadTerrainManifest(baseRevision.GetManifestBytes(), baseManifest) ==
            TerrainCookStatus::Success);

    const terrain::TerrainSampleAddress origin = GetDomainOrigin(base.Layout);
    const int64 borderX = origin.X + PAGE_CELLS;
    const int64 borderZ = origin.Z + PAGE_CELLS;
    Array<uint32> indices;
    REQUIRE(indices.TryResize(PAGE_COUNT));
    usize count = 0;
    const auto collect = [&](terrain::TerrainSampleRegion region) -> TerrainCookStatus {
        return terrain::TryCollectTerrainEditPages(base.Layout, region, indices.AsView(), count);
    };

    // An interior sample reaches one page.
    REQUIRE(collect({{origin.X + 4, origin.Z + 4}, {origin.X + 4, origin.Z + 4}}) == TerrainCookStatus::Success);
    REQUIRE(count == 1);
    CHECK(indices[0] == 0);
    // One sample past the first page's edge is in its halo, so both pages rebuild.
    REQUIRE(collect({{borderX + 1, origin.Z + 4}, {borderX + 1, origin.Z + 4}}) == TerrainCookStatus::Success);
    REQUIRE(count == 2);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    // The four-way corner reaches all four pages around it.
    REQUIRE(collect({{borderX, borderZ}, {borderX, borderZ}}) == TerrainCookStatus::Success);
    REQUIRE(count == 4);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == PAGES_X);
    CHECK(indices[3] == PAGES_X + 1);
    // A region outside the domain reaches nothing; a reversed region is invalid.
    REQUIRE(collect({{origin.X - 10, origin.Z}, {origin.X - 2, origin.Z + 4}}) == TerrainCookStatus::Success);
    CHECK(count == 0);
    count = 3;
    CHECK(collect({{origin.X + 2, origin.Z}, {origin.X + 1, origin.Z}}) == TerrainCookStatus::InvalidArgument);
    CHECK(count == 3);
    // Too little output storage preserves the outputs.
    uint32 single = 99;
    CHECK(terrain::TryCollectTerrainEditPages(base.Layout,
                                              {{borderX, borderZ}, {borderX, borderZ}},
                                              {&single, 1},
                                              count) == TerrainCookStatus::LimitExceeded);
    CHECK(single == 99);
    CHECK(count == 3);

    // A stamp across the first vertical border, near the bottom row of pages.
    const terrain::TerrainSampleRegion stamp{{borderX - 2, origin.Z + 3}, {borderX + 2, origin.Z + 6}};
    Domain edited;
    FillDomain(MakeLayout(), SEED, edited);
    RaiseRegion(edited, stamp);
    edited.Layout.Revision = 2;
    edited.Layout.SourceDigest = {EDITED_SEED, 0};

    terrain::TerrainCookedRevision recooked;
    terrain::TerrainRecookSummary summary;
    REQUIRE(terrain::TryRecookTerrainRevision(baseManifest,
                                              baseRevision.GetPagePackBytes(),
                                              edited.Layout,
                                              edited.Grid,
                                              edited.Elevations.AsView(),
                                              stamp,
                                              recooked,
                                              summary) == TerrainCookStatus::Success);
    CHECK(summary.RecookedPages == 2);
    CHECK(summary.ReusedPages == PAGE_COUNT - 2);

    terrain::TerrainCookedRevision clean;
    CookDomain(edited, clean);
    terrain::TerrainManifest recookedManifest;
    terrain::TerrainManifest cleanManifest;
    Array<terrain::TerrainPage> recookedPages;
    Array<terrain::TerrainPage> cleanPages;
    ReadPages(recooked, recookedManifest, recookedPages);
    ReadPages(clean, cleanManifest, cleanPages);
    CHECK(recookedManifest.GetLayout().Revision == 2);
    CHECK(recookedManifest.GetLayout().SourceDigest == edited.Layout.SourceDigest);
    for (usize i = 0; i < recookedPages.GetSize(); ++i)
    {
        const terrain::TerrainPageRecord& record = recookedManifest.GetPageRecords()[i];
        const terrain::TerrainPageRecord& cleanRecord = cleanManifest.GetPageRecords()[i];
        CHECK(record.PayloadDigest == cleanRecord.PayloadDigest);
        CHECK(record.MinimumCode == cleanRecord.MinimumCode);
        CHECK(record.MaximumCode == cleanRecord.MaximumCode);
        CHECK(
            AreBytesEqual(recookedPages[i].GetTile().GetEncodedHeights(), cleanPages[i].GetTile().GetEncodedHeights()));
        const bool isEdited = i == 0 || i == 1;
        CHECK(record.ContentRevision == (isEdited == true ? 2U : 1U));
        if (isEdited == false)
        {
            CHECK(record.PayloadDigest == baseManifest.GetPageRecords()[i].PayloadDigest);
        }
    }
    // The stamped sample on the shared edge decodes to the raised source height.
    const usize stampedIndex = (4 * static_cast<usize>(edited.Grid.Columns)) + PAGE_CELLS;
    CHECK(math::Abs(DecodeOwnedSample(recookedManifest, recookedPages, {borderX, origin.Z + 4}) -
                    static_cast<float64>(edited.Elevations[stampedIndex])) <= DECODE_TOLERANCE_METRES);

    // The recook rejects a revision that does not advance and a changed layout, preserving its outputs.
    const Array<uint8> kept = CopyBytes(recooked.GetManifestBytes());
    terrain::TerrainRevisionLayout stale = edited.Layout;
    stale.Revision = 1;
    CHECK(terrain::TryRecookTerrainRevision(baseManifest,
                                            baseRevision.GetPagePackBytes(),
                                            stale,
                                            edited.Grid,
                                            edited.Elevations.AsView(),
                                            stamp,
                                            recooked,
                                            summary) == TerrainCookStatus::InvalidArgument);
    terrain::TerrainRevisionLayout moved = edited.Layout;
    moved.Codec.OriginMetres = ORIGIN_METRES - 1;
    CHECK(terrain::TryRecookTerrainRevision(baseManifest,
                                            baseRevision.GetPagePackBytes(),
                                            moved,
                                            edited.Grid,
                                            edited.Elevations.AsView(),
                                            stamp,
                                            recooked,
                                            summary) == TerrainCookStatus::InvalidArgument);
    CHECK(summary.RecookedPages == 2);
    CHECK(AreBytesEqual(recooked.GetManifestBytes(), kept.AsView()));

    // An edit declared too narrowly recooks only the first page; the stale neighbour then disagrees on the shared
    // edge and the recook fails instead of publishing mixed terrain.
    const terrain::TerrainSampleRegion narrow{{borderX - 2, origin.Z + 3}, {borderX - 2, origin.Z + 6}};
    terrain::TerrainCookedRevision rejected;
    CHECK(terrain::TryRecookTerrainRevision(baseManifest,
                                            baseRevision.GetPagePackBytes(),
                                            edited.Layout,
                                            edited.Grid,
                                            edited.Elevations.AsView(),
                                            narrow,
                                            rejected,
                                            summary) == TerrainCookStatus::BorderMismatch);
    CHECK(rejected.IsEmpty() == true);
}

TEST_CASE("Terrain manifests and pages reject corruption, truncation and mismatched ranges", "[terrain][cook]")
{
    Domain domain;
    FillDomain(MakeLayout(), SEED, domain);
    terrain::TerrainCookedRevision revision;
    CookDomain(domain, revision);
    terrain::TerrainManifest manifest;
    REQUIRE(terrain::TryReadTerrainManifest(revision.GetManifestBytes(), manifest) == TerrainCookStatus::Success);
    const Array<uint8> original = CopyBytes(revision.GetManifestBytes());

    const auto readManifest = [](const Array<uint8>& bytes) -> TerrainCookStatus {
        terrain::TerrainManifest result;
        return terrain::TryReadTerrainManifest(bytes.AsView(), result);
    };
    Array<uint8> bytes = CopyBytes(original.AsView());
    bytes[0] = 'X';
    CHECK(readManifest(bytes) == TerrainCookStatus::MalformedData);
    bytes = CopyBytes(original.AsView());
    WriteUint32At(bytes, VERSION_OFFSET, format::FORMAT_VERSION + 1);
    CHECK(readManifest(bytes) == TerrainCookStatus::UnsupportedVersion);
    bytes = CopyBytes(original.AsView());
    bytes.RemoveLast();
    CHECK(readManifest(bytes) == TerrainCookStatus::MalformedData);
    bytes = CopyBytes(original.AsView());
    REQUIRE(bytes.TryAdd(0));
    CHECK(readManifest(bytes) == TerrainCookStatus::MalformedData);
    bytes = CopyBytes(original.AsView());
    bytes[format::MANIFEST_HEADER_BYTES + 1] ^= 1U;
    CHECK(readManifest(bytes) == TerrainCookStatus::DigestMismatch);
    // A digest-consistent record whose range leaves a gap is still malformed.
    bytes = CopyBytes(original.AsView());
    WriteUint64At(bytes, format::MANIFEST_HEADER_BYTES + format::PAGE_RECORD_BYTES + RECORD_OFFSET_FIELD, 1);
    RewriteManifestDigest(bytes);
    CHECK(readManifest(bytes) == TerrainCookStatus::MalformedData);
    // A page count beyond the admitted limit, and a manifest larger than the largest admitted one.
    bytes = CopyBytes(original.AsView());
    WriteUint32At(bytes, PAGE_COUNT_OFFSET, format::MAX_REVISION_PAGES + 1);
    RewriteManifestDigest(bytes);
    CHECK(readManifest(bytes) == TerrainCookStatus::LimitExceeded);
    bytes = CopyBytes(original.AsView());
    REQUIRE(bytes.TryResize(format::MAX_MANIFEST_BYTES + 1));
    CHECK(readManifest(bytes) == TerrainCookStatus::LimitExceeded);
    CHECK(readManifest(Array<uint8>{}) == TerrainCookStatus::MalformedData);

    // Pages: corruption, truncation and a range from another page, each preserving the previous output.
    terrain::TerrainPage page;
    const terrain::TerrainPageRecord& first = manifest.GetPageRecords()[0];
    REQUIRE(terrain::TryReadTerrainPage(manifest, 0, GetPageRange(revision, first), page) ==
            TerrainCookStatus::Success);
    const Fingerprint128 kept = page.GetRecord().PayloadDigest;
    Array<uint8> blob = CopyBytes(GetPageRange(revision, first));
    blob[format::PAGE_HEADER_BYTES + 3] ^= 1U;
    CHECK(terrain::TryReadTerrainPage(manifest, 0, blob.AsView(), page) == TerrainCookStatus::DigestMismatch);
    blob = CopyBytes(GetPageRange(revision, first));
    blob.RemoveLast();
    CHECK(terrain::TryReadTerrainPage(manifest, 0, blob.AsView(), page) == TerrainCookStatus::MalformedData);
    blob = CopyBytes(GetPageRange(revision, first));
    blob[1] = 'X';
    CHECK(terrain::TryReadTerrainPage(manifest, 0, blob.AsView(), page) == TerrainCookStatus::MalformedData);
    blob = CopyBytes(GetPageRange(revision, first));
    WriteUint32At(blob, VERSION_OFFSET, format::FORMAT_VERSION + 1);
    CHECK(terrain::TryReadTerrainPage(manifest, 0, blob.AsView(), page) == TerrainCookStatus::UnsupportedVersion);
    CHECK(terrain::TryReadTerrainPage(manifest, 0, GetPageRange(revision, manifest.GetPageRecords()[1]), page) ==
          TerrainCookStatus::PageMismatch);
    CHECK(terrain::TryReadTerrainPage(manifest, PAGE_COUNT, GetPageRange(revision, first), page) ==
          TerrainCookStatus::InvalidArgument);
    const terrain::TerrainManifest unread;
    CHECK(terrain::TryReadTerrainPage(unread, 0, GetPageRange(revision, first), page) ==
          TerrainCookStatus::InvalidArgument);
    CHECK(page.GetRecord().PayloadDigest == kept);
    CHECK(page.GetRecord().Address.X == first.Address.X);

    // Extraction needs the complete pack and validates the page it copies.
    terrain::TerrainCookedPage extracted;
    const BorrowedView<const uint8> pack = revision.GetPagePackBytes();
    CHECK(terrain::TryExtractTerrainCookedPage(manifest, {pack.GetDataOrNull(), pack.GetSize() - 1}, 0, extracted) ==
          TerrainCookStatus::MalformedData);
    Array<uint8> corruptPack = CopyBytes(pack);
    corruptPack[format::PAGE_HEADER_BYTES] ^= 1U;
    CHECK(terrain::TryExtractTerrainCookedPage(manifest, corruptPack.AsView(), 0, extracted) ==
          TerrainCookStatus::DigestMismatch);
    CHECK(extracted.IsEmpty() == true);
    REQUIRE(terrain::TryExtractTerrainCookedPage(manifest, pack, 1, extracted) == TerrainCookStatus::Success);
    CHECK(extracted.GetRecord().PayloadDigest == manifest.GetPageRecords()[1].PayloadDigest);
}

TEST_CASE("Terrain cooking enforces layout, field, codec and size limits", "[terrain][cook]")
{
    Domain domain;
    FillDomain(MakeLayout(), SEED, domain);
    terrain::TerrainCookedRevision revision;
    const auto cook = [&](const terrain::TerrainRevisionLayout& layout) -> TerrainCookStatus {
        return terrain::TryCookTerrainRevision(layout, domain.Grid, domain.Elevations.AsView(), revision);
    };

    terrain::TerrainRevisionLayout layout = domain.Layout;
    layout.Codec.QuantumMetres = 1.0F / 1024.0F; // A 64 m span cannot hold heights up to 120 m.
    CHECK(cook(layout) == TerrainCookStatus::HeightOutOfRange);
    layout = domain.Layout;
    layout.CellsPerPage = format::MAX_PAGE_AXIS_CELLS + 1;
    CHECK(cook(layout) == TerrainCookStatus::InvalidArgument);
    layout = domain.Layout;
    layout.PagesX = (format::MAX_DOMAIN_AXIS_CELLS / PAGE_CELLS) + 1;
    CHECK(cook(layout) == TerrainCookStatus::LimitExceeded);
    layout = domain.Layout;
    layout.CellsPerPage = 1;
    layout.PagesX = 64;
    layout.PagesZ = 65;
    CHECK(cook(layout) == TerrainCookStatus::LimitExceeded);
    layout = domain.Layout;
    layout.FirstPage.X = 0x7fffffffffffffff / PAGE_CELLS;
    CHECK(cook(layout) == TerrainCookStatus::CoordinateOutOfRange);
    layout = domain.Layout;
    layout.SpacingMetres = 2;
    CHECK(cook(layout) == TerrainCookStatus::InvalidArgument);
    layout = domain.Layout;
    layout.PagesX = PAGES_X - 1;
    CHECK(cook(layout) == TerrainCookStatus::InvalidArgument);
    CHECK(revision.IsEmpty() == true);

    CHECK(terrain::TryCookTerrainRevision(domain.Layout,
                                          domain.Grid,
                                          {domain.Elevations.GetData(), domain.Elevations.GetSize() - 1},
                                          revision) == TerrainCookStatus::InvalidStorage);
    terrain::TerrainCookedPage page;
    CHECK(terrain::TryCookTerrainPage(domain.Layout,
                                      domain.Grid,
                                      domain.Elevations.AsView(),
                                      {domain.Layout.FirstPage.X + PAGES_X, domain.Layout.FirstPage.Z},
                                      page) == TerrainCookStatus::InvalidArgument);
    CHECK(page.IsEmpty() == true);

    // A 32-bit codec with a finer quantum round-trips through the same reader.
    layout = domain.Layout;
    layout.Codec =
    {
        .Encoding = terrain::TerrainHeightEncoding::Signed32,
        .OriginMetres = 0,
        .QuantumMetres = 1.0F / 4096.0F,
    };
    REQUIRE(cook(layout) == TerrainCookStatus::Success);
    terrain::TerrainManifest manifest;
    Array<terrain::TerrainPage> pages;
    ReadPages(revision, manifest, pages);
    CHECK(manifest.GetLayout().Codec.Encoding == terrain::TerrainHeightEncoding::Signed32);
    CHECK(pages[0].GetRecord().ByteSize == format::GetPageBlobBytes(format::HeightEncodingTag::Signed32, PAGE_CELLS));
    float64 height = 0;
    REQUIRE(terrain::TryGetTerrainPageHeight(pages[0], 3, 5, height) == TerrainCookStatus::Success);
    CHECK(math::Abs(height - static_cast<float64>(domain.Elevations[(5 * domain.Grid.Columns) + 3])) <=
          (0.5 / 4096.0) + 1.0e-5);
}
