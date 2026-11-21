#pragma once

// Golden LUFOL page set shared by the native FoliagePages tests and the
// WebAssembly math probe (tools/web-math-probe), so native and browser builds
// must encode identical bytes. Header-only and free of the test framework.
//
// The fixture crosses the negative-coordinate seam at page (-1, -1) / (-1, 0),
// includes an empty page, roots on both edges of a half-open page square, two
// lattices and two species.

#include <ludus/foundation/base/core.h>

#include <ludus/content/foliage_pages/foliage_pages.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::foliage_pages::golden
{
/// Page edge: 32 terrain samples.
inline constexpr uint32 PAGE_SIZE = 8192;
/// Persistent terrain domain of the fixture.
inline constexpr uint64 DOMAIN_ID = 0x464f4c0000000001ULL;
/// Terrain revision of the fixture.
inline constexpr uint64 TERRAIN_REVISION = 7;
/// Generator version of the fixture.
inline constexpr uint32 GENERATOR_VERSION = 1;
/// Terrain sample column of the lattice origin.
inline constexpr int64 ORIGIN_X = -1024;
/// Terrain sample row of the lattice origin.
inline constexpr int64 ORIGIN_Z = 2048;
/// Multiplier of the fixture's input digest pattern.
inline constexpr uint8 DIGEST_STEP = 7;

/// Layers of the fixture: 8-sample tree cells and 2-sample understory cells.
inline constexpr FoliageLayer LAYERS[] = {
    { .Id = 1, .SlotsPerCell = 4, .CellSize = 2048 },
    { .Id = 3, .SlotsPerCell = 8, .CellSize = 512 },
};

/// Species of the fixture.
inline constexpr FoliageSpecies SPECIES[] = {
    { .Key = 0x1001, .PlacementRevision = 5, .VariantCount = 3 },
    { .Key = 0x2002, .PlacementRevision = 9, .VariantCount = 1 },
};

/// Page directory of the fixture, in (Z, X) order.
inline constexpr FoliagePage PAGES[] = {
    {
        .PageX = -1,
        .PageZ = -1,
        .FirstPlacement = 0,
        .PlacementCount = 3,
        .Bounds =
        {
            .MinimumX = -8300,
            .MinimumZ = -8300,
            .MaximumX = 100,
            .MaximumZ = 100,
            .MinimumHeight = -400,
            .MaximumHeight = 60000,
        },
    },
    { .PageX = 0, .PageZ = -1, .FirstPlacement = 3, .PlacementCount = 0 },
    {
        .PageX = -1,
        .PageZ = 0,
        .FirstPlacement = 3,
        .PlacementCount = 2,
        .Bounds =
        {
            .MinimumX = -3200,
            .MinimumZ = -100,
            .MaximumX = 100,
            .MaximumZ = 8300,
            .MinimumHeight = 900,
            .MaximumHeight = 2200,
        },
    },
};

/// Placements of the fixture, in page runs and identity order.
inline constexpr FoliagePlacement PLACEMENTS[] = {
    // Page (-1, -1): roots at X = -8092, Z = -6145.
    {
        .Layer = 1,
        .Slot = 0,
        .CellX = -4,
        .CellZ = -4,
        .OffsetX = 100,
        .OffsetZ = 2047,
        .Height = 51200,
        .Yaw = 12345,
        .Scale = 4096,
        .Species = 0,
        .Variant = 2,
    },
    // X = -1 and Z = -8192: the last and first lattice units the page owns.
    {
        .Layer = 1,
        .Slot = 3,
        .CellX = -1,
        .CellZ = -4,
        .OffsetX = 2047,
        .OffsetZ = 0,
        .Height = -300,
        .Yaw = 65535,
        .Scale = 6144,
        .NormalX = 1200,
        .NormalZ = -800,
        .Species = 0,
        .Variant = 0,
    },
    {
        .Layer = 3,
        .Slot = 7,
        .CellX = -16,
        .CellZ = -1,
        .OffsetX = 5,
        .OffsetZ = 511,
        .Height = 0,
        .Yaw = 0,
        .Scale = 1,
        .NormalX = -32767,
        .NormalZ = 0,
        .Species = 1,
        .Variant = 0,
    },
    // Page (-1, 0): Z = 0 is the first row the page owns.
    {
        .Layer = 1,
        .Slot = 1,
        .CellX = -2,
        .CellZ = 0,
        .OffsetX = 1024,
        .OffsetZ = 0,
        .Height = 1024,
        .Yaw = 32768,
        .Scale = 4095,
        .NormalX = 0,
        .NormalZ = 32767,
        .Species = 1,
        .Variant = 0,
    },
    {
        .Layer = 3,
        .Slot = 0,
        .CellX = -1,
        .CellZ = 15,
        .OffsetX = 511,
        .OffsetZ = 511,
        .Height = 2048,
        .Yaw = 1,
        .Scale = 65535,
        .NormalX = 23169,
        .NormalZ = 23169,
        .Species = 0,
        .Variant = 1,
    },
};

/// Encoded size of the fixture for LUFOL version 1.
inline constexpr usize ENCODED_BYTES = 488;
/// FNV-1a offset basis.
inline constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
/// FNV-1a prime.
inline constexpr uint64 FNV_PRIME = 0x100000001b3ULL;
/// Pinned FNV-1a digest of the fixture's bytes for LUFOL version 1. A different value changes every cooked page
/// set and requires a new format or generator version, never an updated constant.
inline constexpr uint64 ENCODED_DIGEST = 0x2a8fa31f9f8e90adULL;

/// Fixture header with its digest pattern.
inline FoliagePageSetHeader MakeHeader() noexcept
{
    FoliagePageSetHeader header;
    header.GeneratorVersion = GENERATOR_VERSION;
    header.PageSize = PAGE_SIZE;
    header.DomainId = DOMAIN_ID;
    header.OriginX = ORIGIN_X;
    header.OriginZ = ORIGIN_Z;
    header.TerrainRevision = TERRAIN_REVISION;
    for (usize i = 0; i < header.InputDigest.GetSize(); ++i)
    {
        header.InputDigest[i] = static_cast<uint8>(i * DIGEST_STEP + 1);
    }
    return header;
}

/// Fixture description borrowing the constant tables above.
inline FoliagePageSetDescription MakeDescription() noexcept
{
    FoliagePageSetDescription description;
    description.Header = MakeHeader();
    description.Layers = foundation::BorrowedView<const FoliageLayer>(LAYERS);
    description.Species = foundation::BorrowedView<const FoliageSpecies>(SPECIES);
    description.Pages = foundation::BorrowedView<const FoliagePage>(PAGES);
    description.Placements = foundation::BorrowedView<const FoliagePlacement>(PLACEMENTS);
    return description;
}

/// Binding that matches the fixture.
inline constexpr FoliagePageBinding BINDING = { .DomainId = DOMAIN_ID, .TerrainRevision = TERRAIN_REVISION };

/// FNV-1a digest of a byte range.
inline uint64 ComputeDigest(foundation::BorrowedView<const uint8> bytes) noexcept
{
    uint64 digest = FNV_OFFSET_BASIS;
    for (const uint8 value : bytes)
    {
        digest = (digest ^ value) * FNV_PRIME;
    }
    return digest;
}
} // namespace ludus::content::foliage_pages::golden
