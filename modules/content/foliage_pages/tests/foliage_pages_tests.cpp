// Encoder and validating reader for LUFOL foliage placement page sets.

#include <ludus/content/foliage_pages/foliage_pages.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "foliage_pages_golden.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::foliage_pages;
namespace format = ludus::content::foliage_format;
using ludus::foundation::BorrowedView;
using ludus::foundation::StaticArray;

constexpr usize BUFFER_BYTES = 1024;
constexpr uint8 FILL_BYTE = 0xa5;
constexpr usize LAYER_COUNT = sizeof(golden::LAYERS) / sizeof(golden::LAYERS[0]);
constexpr usize SPECIES_COUNT = sizeof(golden::SPECIES) / sizeof(golden::SPECIES[0]);
constexpr usize PAGE_COUNT = sizeof(golden::PAGES) / sizeof(golden::PAGES[0]);
constexpr usize PLACEMENT_COUNT = sizeof(golden::PLACEMENTS) / sizeof(golden::PLACEMENTS[0]);

using Buffer = StaticArray<uint8, BUFFER_BYTES>;

/// Mutable copy of the golden tables for rule-by-rule rejection tests.
struct EditableSet final
{
    FoliagePageSetHeader Header = golden::MakeHeader();
    StaticArray<FoliageLayer, LAYER_COUNT> Layers{};
    StaticArray<FoliageSpecies, SPECIES_COUNT> Species{};
    StaticArray<FoliagePage, PAGE_COUNT> Pages{};
    StaticArray<FoliagePlacement, PLACEMENT_COUNT> Placements{};

    EditableSet() noexcept
    {
        for (usize i = 0; i < LAYER_COUNT; ++i)
        {
            Layers[i] = golden::LAYERS[i];
        }
        for (usize i = 0; i < SPECIES_COUNT; ++i)
        {
            Species[i] = golden::SPECIES[i];
        }
        for (usize i = 0; i < PAGE_COUNT; ++i)
        {
            Pages[i] = golden::PAGES[i];
        }
        for (usize i = 0; i < PLACEMENT_COUNT; ++i)
        {
            Placements[i] = golden::PLACEMENTS[i];
        }
    }

    [[nodiscard]] FoliagePageSetDescription Describe() const noexcept
    {
        FoliagePageSetDescription description;
        description.Header = Header;
        description.Layers = BorrowedView<const FoliageLayer>(Layers.GetData(), Layers.GetSize());
        description.Species = BorrowedView<const FoliageSpecies>(Species.GetData(), Species.GetSize());
        description.Pages = BorrowedView<const FoliagePage>(Pages.GetData(), Pages.GetSize());
        description.Placements = BorrowedView<const FoliagePlacement>(Placements.GetData(), Placements.GetSize());
        return description;
    }
};

Buffer MakeFilledBuffer() noexcept
{
    Buffer buffer{};
    for (usize i = 0; i < buffer.GetSize(); ++i)
    {
        buffer[i] = FILL_BYTE;
    }
    return buffer;
}

FoliagePageStatus Encode(const FoliagePageSetDescription& description,
                         Buffer& outBuffer,
                         usize& outBytes,
                         FoliagePageDiagnostic& outDiagnostic,
                         const FoliagePageLimits& limits = {})
{
    return WriteFoliagePageSet(description,
                               limits,
                               BorrowedView<uint8>(outBuffer.GetData(), outBuffer.GetSize()),
                               outBytes,
                               outDiagnostic);
}

usize EncodeGolden(Buffer& outBuffer)
{
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Encode(golden::MakeDescription(), outBuffer, bytes, diagnostic) == FoliagePageStatus::Ok);
    return bytes;
}

FoliagePageStatus Read(const Buffer& buffer,
                       usize bytes,
                       FoliagePageSetView& outView,
                       FoliagePageDiagnostic& outDiagnostic,
                       const FoliagePageLimits& limits = {},
                       const FoliagePageBinding& binding = golden::BINDING)
{
    return ReadFoliagePageSet(BorrowedView<const uint8>(buffer.GetData(), bytes),
                              binding,
                              limits,
                              outView,
                              outDiagnostic);
}

void WriteWord(Buffer& inoutBuffer, usize offset, uint32 value)
{
    for (usize i = 0; i < sizeof(uint32); ++i)
    {
        inoutBuffer[offset + i] = static_cast<uint8>(value >> (i * 8));
    }
}

void WriteHalf(Buffer& inoutBuffer, usize offset, uint16 value)
{
    inoutBuffer[offset] = static_cast<uint8>(value);
    inoutBuffer[offset + 1] = static_cast<uint8>(value >> 8);
}

usize GetPlacementTableOffset()
{
    return format::HEADER_BYTES + LAYER_COUNT * format::LAYER_RECORD_BYTES +
           SPECIES_COUNT * format::SPECIES_RECORD_BYTES + PAGE_COUNT * format::PAGE_RECORD_BYTES;
}

/// Re-encodes everything a view exposes; canonical page sets reproduce their bytes.
usize ReencodeView(const FoliagePageSetView& view, Buffer& outBuffer)
{
    StaticArray<FoliageLayer, format::MAX_LAYERS> layers{};
    StaticArray<FoliageSpecies, SPECIES_COUNT> species{};
    StaticArray<FoliagePage, PAGE_COUNT> pages{};
    StaticArray<FoliagePlacement, PLACEMENT_COUNT> placements{};
    REQUIRE(view.GetSpeciesCount() <= species.GetSize());
    REQUIRE(view.GetPageCount() <= pages.GetSize());
    REQUIRE(view.GetPlacementCount() <= placements.GetSize());
    for (usize i = 0; i < view.GetLayerCount(); ++i)
    {
        REQUIRE(view.TryGetLayer(i, layers[i]) == FoliagePageStatus::Ok);
    }
    for (usize i = 0; i < view.GetSpeciesCount(); ++i)
    {
        REQUIRE(view.TryGetSpecies(i, species[i]) == FoliagePageStatus::Ok);
    }
    for (usize i = 0; i < view.GetPageCount(); ++i)
    {
        REQUIRE(view.TryGetPage(i, pages[i]) == FoliagePageStatus::Ok);
    }
    for (usize i = 0; i < view.GetPlacementCount(); ++i)
    {
        REQUIRE(view.TryGetPlacement(i, placements[i]) == FoliagePageStatus::Ok);
    }
    FoliagePageSetDescription description;
    description.Header = view.GetHeader();
    description.Layers = BorrowedView<const FoliageLayer>(layers.GetData(), view.GetLayerCount());
    description.Species = BorrowedView<const FoliageSpecies>(species.GetData(), view.GetSpeciesCount());
    description.Pages = BorrowedView<const FoliagePage>(pages.GetData(), view.GetPageCount());
    description.Placements = BorrowedView<const FoliagePlacement>(placements.GetData(), view.GetPlacementCount());
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Encode(description, outBuffer, bytes, diagnostic) == FoliagePageStatus::Ok);
    return bytes;
}

void RequireRejected(const EditableSet& set, FoliagePageStatus status, FoliagePageIssue issue, usize index)
{
    Buffer buffer = MakeFilledBuffer();
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Encode(set.Describe(), buffer, bytes, diagnostic) == status);
    CHECK(diagnostic.Status == status);
    CHECK(diagnostic.Issue == issue);
    CHECK(diagnostic.Index == index);
    CHECK(bytes == 0);
    for (usize i = 0; i < buffer.GetSize(); ++i)
    {
        REQUIRE(buffer[i] == FILL_BYTE);
    }
}
} // namespace

TEST_CASE("The golden page set encodes to pinned bytes", "[foliage_pages]")
{
    Buffer buffer{};
    const usize bytes = EncodeGolden(buffer);
    REQUIRE(bytes == golden::ENCODED_BYTES);
    CHECK(golden::ComputeDigest(BorrowedView<const uint8>(buffer.GetData(), bytes)) == golden::ENCODED_DIGEST);

    usize measured = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(GetFoliagePageSetBytes(golden::MakeDescription(), {}, measured, diagnostic) == FoliagePageStatus::Ok);
    CHECK(measured == bytes);

    // Little-endian field spellings: OriginX is -1024 and the first placement's CellX is -4.
    constexpr uint8 ORIGIN_X_BYTES[] = {0x00, 0xfc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    for (usize i = 0; i < sizeof(ORIGIN_X_BYTES); ++i)
    {
        CHECK(buffer[format::ORIGIN_X_OFFSET + i] == ORIGIN_X_BYTES[i]);
    }
    constexpr uint8 FIRST_PLACEMENT_BYTES[] =
        {0x01, 0x00, 0x00, 0x00, 0xfc, 0xff, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xff, 0x64, 0x00, 0xff, 0x07};
    for (usize i = 0; i < sizeof(FIRST_PLACEMENT_BYTES); ++i)
    {
        CHECK(buffer[GetPlacementTableOffset() + i] == FIRST_PLACEMENT_BYTES[i]);
    }
}

TEST_CASE("A read page set exposes the encoded records and re-encodes identically", "[foliage_pages]")
{
    Buffer buffer{};
    const usize bytes = EncodeGolden(buffer);
    FoliagePageSetView view;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Read(buffer, bytes, view, diagnostic) == FoliagePageStatus::Ok);
    CHECK(diagnostic.Status == FoliagePageStatus::Ok);
    REQUIRE(view.IsValid() == true);
    REQUIRE(view.GetPlacementCount() == PLACEMENT_COUNT);

    const FoliagePageSetHeader header = view.GetHeader();
    CHECK(header.GeneratorVersion == golden::GENERATOR_VERSION);
    CHECK(header.OriginX == golden::ORIGIN_X);
    CHECK(header.OriginZ == golden::ORIGIN_Z);
    CHECK(header.InputDigest[1] == golden::DIGEST_STEP + 1);
    for (usize i = 0; i < PLACEMENT_COUNT; ++i)
    {
        FoliagePlacement placement;
        REQUIRE(view.TryGetPlacement(i, placement) == FoliagePageStatus::Ok);
        CHECK(placement == golden::PLACEMENTS[i]);
    }
    FoliagePage page;
    REQUIRE(view.TryGetPage(2, page) == FoliagePageStatus::Ok);
    CHECK(page.Bounds == golden::PAGES[2].Bounds);

    Buffer reencoded{};
    REQUIRE(ReencodeView(view, reencoded) == bytes);
    for (usize i = 0; i < bytes; ++i)
    {
        REQUIRE(reencoded[i] == buffer[i]);
    }
}

TEST_CASE("Placement roots follow the cell lattice across negative coordinates", "[foliage_pages]")
{
    const FoliageRoot edge = ComputeFoliageRoot(golden::LAYERS[0].CellSize, golden::PLACEMENTS[1]);
    CHECK(edge.X == -1);
    CHECK(edge.Z == -static_cast<int64>(golden::PAGE_SIZE));
    const FoliageRoot seam = ComputeFoliageRoot(golden::LAYERS[0].CellSize, golden::PLACEMENTS[3]);
    CHECK(seam.Z == 0);
}

TEST_CASE("Pages are found by address", "[foliage_pages]")
{
    Buffer buffer{};
    const usize bytes = EncodeGolden(buffer);
    FoliagePageSetView view;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Read(buffer, bytes, view, diagnostic) == FoliagePageStatus::Ok);
    for (usize i = 0; i < PAGE_COUNT; ++i)
    {
        usize index = PAGE_COUNT;
        REQUIRE(view.TryFindPage(golden::PAGES[i].PageX, golden::PAGES[i].PageZ, index) == FoliagePageStatus::Ok);
        CHECK(index == i);
    }
    usize untouched = PAGE_COUNT;
    CHECK(view.TryFindPage(0, 0, untouched) == FoliagePageStatus::NotFound);
    CHECK(view.TryFindPage(-2, -1, untouched) == FoliagePageStatus::NotFound);
    CHECK(untouched == PAGE_COUNT);

    const FoliagePageSetView empty;
    FoliagePlacement placement;
    CHECK(empty.IsValid() == false);
    CHECK(empty.TryFindPage(0, 0, untouched) == FoliagePageStatus::InvalidHandle);
    CHECK(empty.TryGetPlacement(0, placement) == FoliagePageStatus::InvalidHandle);
    CHECK(view.TryGetPlacement(PLACEMENT_COUNT, placement) == FoliagePageStatus::InvalidHandle);
}

TEST_CASE("An empty page set round trips", "[foliage_pages]")
{
    FoliagePageSetDescription description;
    description.Header = golden::MakeHeader();
    Buffer buffer{};
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Encode(description, buffer, bytes, diagnostic) == FoliagePageStatus::Ok);
    REQUIRE(bytes == format::HEADER_BYTES);
    FoliagePageSetView view;
    REQUIRE(Read(buffer, bytes, view, diagnostic) == FoliagePageStatus::Ok);
    CHECK(view.GetPageCount() == 0);
}

TEST_CASE("Coordinates at the signed lattice limits are admitted exactly", "[foliage_pages]")
{
    constexpr int32 MAXIMUM = 0x7fffffff;
    constexpr int32 MINIMUM = -MAXIMUM - 1;
    const FoliageLayer layers[] = {{ .Id = 1, .SlotsPerCell = 1, .CellSize = format::MAX_CELL_SIZE }};
    const FoliageSpecies species[] = {{ .Key = 1, .VariantCount = 1 }};
    const FoliagePlacement placements[] = {
        { .Layer = 1, .CellX = MINIMUM, .CellZ = MINIMUM, .OffsetX = 0, .OffsetZ = 0, .Scale = 1 },
        { .Layer = 1, .CellX = MAXIMUM, .CellZ = MAXIMUM, .OffsetX = 0xffff, .OffsetZ = 0xffff, .Scale = 1 },
    };
    const FoliageRoot lowest = ComputeFoliageRoot(format::MAX_CELL_SIZE, placements[0]);
    const FoliageRoot highest = ComputeFoliageRoot(format::MAX_CELL_SIZE, placements[1]);
    const FoliagePage pages[] = {
        {
            .PageX = MINIMUM,
            .PageZ = MINIMUM,
            .FirstPlacement = 0,
            .PlacementCount = 1,
            .Bounds = { .MinimumX = lowest.X, .MinimumZ = lowest.Z, .MaximumX = lowest.X, .MaximumZ = lowest.Z },
        },
        {
            .PageX = MAXIMUM,
            .PageZ = MAXIMUM,
            .FirstPlacement = 1,
            .PlacementCount = 1,
            .Bounds = { .MinimumX = highest.X, .MinimumZ = highest.Z, .MaximumX = highest.X, .MaximumZ = highest.Z },
        },
    };
    CHECK(lowest.X == -(int64{1} << 47));
    CHECK(highest.X == (int64{1} << 47) - 1);

    FoliagePageSetDescription description;
    description.Header = golden::MakeHeader();
    description.Header.PageSize = format::MAX_CELL_SIZE;
    description.Layers = BorrowedView<const FoliageLayer>(layers);
    description.Species = BorrowedView<const FoliageSpecies>(species);
    description.Pages = BorrowedView<const FoliagePage>(pages);
    description.Placements = BorrowedView<const FoliagePlacement>(placements);
    Buffer buffer{};
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Encode(description, buffer, bytes, diagnostic) == FoliagePageStatus::Ok);
    FoliagePageSetView view;
    REQUIRE(Read(buffer, bytes, view, diagnostic) == FoliagePageStatus::Ok);
    FoliagePlacement decoded;
    REQUIRE(view.TryGetPlacement(1, decoded) == FoliagePageStatus::Ok);
    CHECK(decoded == placements[1]);
}

TEST_CASE("The encoder rejects each rule before writing", "[foliage_pages]")
{
    SECTION("header")
    {
        EditableSet set;
        set.Header.GeneratorVersion = 0;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::GeneratorVersion, 0);
        set = EditableSet{};
        set.Header.DomainId = 0;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::DomainId, 0);
        set = EditableSet{};
        set.Header.PageSize = format::MAX_PAGE_SIZE + 1;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PageSize, 0);
    }
    SECTION("layers")
    {
        EditableSet set;
        set.Layers[1].Id = set.Layers[0].Id;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::LayerOrder, 1);
        set = EditableSet{};
        set.Layers[0].SlotsPerCell = format::MAX_SLOTS_PER_CELL + 1;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::LayerSlots, 0);
        set = EditableSet{};
        set.Layers[1].CellSize = 3000; // Does not divide the 8192 page size.
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::LayerCellSize, 1);
    }
    SECTION("species")
    {
        EditableSet set;
        set.Species[0].Key = 0;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::SpeciesKey, 0);
        set = EditableSet{};
        set.Species[1].Key = set.Species[0].Key;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::SpeciesKey, 1);
        set = EditableSet{};
        set.Species[1].VariantCount = 0;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::SpeciesVariants, 1);
    }
    SECTION("pages")
    {
        EditableSet set;
        set.Pages[1].PageX = -1; // Repeats page (-1, -1).
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PageOrder, 1);
        set = EditableSet{};
        set.Pages[2].FirstPlacement = 2;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PageRange, 2);
        set = EditableSet{};
        set.Pages[2].PlacementCount = 1; // Leaves the last placement without a page.
        set.Pages[2].Bounds.MaximumZ = 8191;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementCount, 0);
        set = EditableSet{};
        set.Pages[1].Bounds.MaximumX = 1;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PageBounds, 1);
        set = EditableSet{};
        set.Pages[0].Bounds.MinimumHeight = set.Pages[0].Bounds.MaximumHeight + 1;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PageBounds, 0);
    }
    SECTION("placements")
    {
        EditableSet set;
        set.Placements[0].Layer = 2;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementLayer, 0);
        set = EditableSet{};
        set.Placements[2].Slot = 8;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementSlot, 2);
        set = EditableSet{};
        set.Placements[4].OffsetX = 512;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementOffset, 4);
        set = EditableSet{};
        set.Placements[1].CellX = 0; // Root X = 2047 lies in page (0, -1).
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementPage, 1);
        set = EditableSet{};
        set.Placements[1] = set.Placements[0]; // Duplicate key.
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementOrder, 1);
        set = EditableSet{};
        set.Placements[3].Scale = 0;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementScale, 3);
        set = EditableSet{};
        set.Placements[4].NormalX = 23170;
        set.Placements[4].NormalZ = 23170;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementNormal, 4);
        set = EditableSet{};
        set.Placements[0].NormalX = -0x7fff - 1;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementNormal, 0);
        set = EditableSet{};
        set.Placements[0].Species = 2;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementSpecies, 0);
        set = EditableSet{};
        set.Placements[2].Variant = 1; // Species 1 has one variant.
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementVariant, 2);
        set = EditableSet{};
        set.Placements[0].Height = 60001;
        RequireRejected(set, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementBounds, 0);
    }
}

TEST_CASE("Over-budget page sets are rejected against the configured limits", "[foliage_pages]")
{
    const EditableSet set;
    FoliagePageLimits limits;
    limits.MaximumBytes = golden::ENCODED_BYTES - 1;
    Buffer buffer = MakeFilledBuffer();
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    CHECK(Encode(set.Describe(), buffer, bytes, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::Size);

    limits = {};
    limits.MaximumPlacements = PLACEMENT_COUNT - 1;
    CHECK(Encode(set.Describe(), buffer, bytes, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::PlacementCount);

    limits = {};
    limits.MaximumSpecies = 1;
    CHECK(Encode(set.Describe(), buffer, bytes, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::SpeciesCount);

    limits = {};
    limits.MaximumPlacementsPerPage = 2;
    CHECK(Encode(set.Describe(), buffer, bytes, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::PagePlacementCount);
    CHECK(diagnostic.Index == 0);

    limits = {};
    limits.MaximumPages = format::MAX_PAGES + 1;
    CHECK(Encode(set.Describe(), buffer, bytes, diagnostic, limits) == FoliagePageStatus::InvalidArgument);
    CHECK(diagnostic.Issue == FoliagePageIssue::Limits);
    CHECK(bytes == 0);
    for (usize i = 0; i < buffer.GetSize(); ++i)
    {
        REQUIRE(buffer[i] == FILL_BYTE);
    }

    // The reader applies the same limits to encoded bytes, before reading past the byte count.
    const usize encoded = EncodeGolden(buffer);
    FoliagePageSetView view;
    limits = {};
    limits.MaximumBytes = encoded - 1;
    CHECK(Read(buffer, encoded, view, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::Size);
    limits = {};
    limits.MaximumLayers = 1;
    CHECK(Read(buffer, encoded, view, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::LayerCount);
    CHECK(diagnostic.Offset == format::LAYER_COUNT_OFFSET);
    limits = {};
    limits.MaximumPlacementsPerPage = 2;
    CHECK(Read(buffer, encoded, view, diagnostic, limits) == FoliagePageStatus::LimitExceeded);
    CHECK(diagnostic.Issue == FoliagePageIssue::PagePlacementCount);
    CHECK(view.IsValid() == false);
}

TEST_CASE("The encoder reports insufficient output storage without writing", "[foliage_pages]")
{
    Buffer buffer = MakeFilledBuffer();
    usize bytes = 0;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(WriteFoliagePageSet(golden::MakeDescription(),
                                {},
                                BorrowedView<uint8>(buffer.GetData(), golden::ENCODED_BYTES - 1),
                                bytes,
                                diagnostic) == FoliagePageStatus::CapacityExceeded);
    CHECK(diagnostic.Offset == golden::ENCODED_BYTES);
    for (usize i = 0; i < buffer.GetSize(); ++i)
    {
        REQUIRE(buffer[i] == FILL_BYTE);
    }
    CHECK(WriteFoliagePageSet(golden::MakeDescription(), {}, BorrowedView<uint8>(nullptr, 1), bytes, diagnostic) ==
          FoliagePageStatus::InvalidArgument);
    FoliagePageSetDescription invalid = golden::MakeDescription();
    invalid.Placements = BorrowedView<const FoliagePlacement>(nullptr, 1);
    CHECK(GetFoliagePageSetBytes(invalid, {}, bytes, diagnostic) == FoliagePageStatus::InvalidArgument);
    CHECK(diagnostic.Issue == FoliagePageIssue::InputView);
    CHECK(bytes == 0);
}

TEST_CASE("The reader rejects malformed bytes and keeps the previous view", "[foliage_pages]")
{
    Buffer buffer{};
    const usize bytes = EncodeGolden(buffer);
    FoliagePageSetView previous;
    FoliagePageDiagnostic diagnostic;
    REQUIRE(Read(buffer, bytes, previous, diagnostic) == FoliagePageStatus::Ok);

    const auto requireRejected = [&](const Buffer& candidate,
                                     usize size,
                                     FoliagePageStatus status,
                                     FoliagePageIssue issue,
                                     const FoliagePageBinding& binding = golden::BINDING) {
        FoliagePageSetView view = previous;
        FoliagePageDiagnostic failure;
        REQUIRE(Read(candidate, size, view, failure, {}, binding) == status);
        CHECK(failure.Issue == issue);
        CHECK(view.GetPlacementCount() == previous.GetPlacementCount());
        FoliagePlacement placement;
        REQUIRE(view.TryGetPlacement(0, placement) == FoliagePageStatus::Ok);
        CHECK(placement == golden::PLACEMENTS[0]);
    };

    requireRejected(buffer, format::HEADER_BYTES - 1, FoliagePageStatus::Malformed, FoliagePageIssue::Size);
    requireRejected(buffer, bytes - 1, FoliagePageStatus::Malformed, FoliagePageIssue::Size);
    requireRejected(buffer, bytes + 1, FoliagePageStatus::Malformed, FoliagePageIssue::Size);

    Buffer corrupt = buffer;
    corrupt[0] = 'X';
    requireRejected(corrupt, bytes, FoliagePageStatus::Malformed, FoliagePageIssue::Magic);
    corrupt = buffer;
    WriteWord(corrupt, format::FORMAT_VERSION_OFFSET, format::FORMAT_VERSION_1 + 1);
    requireRejected(corrupt, bytes, FoliagePageStatus::UnsupportedVersion, FoliagePageIssue::FormatVersion);
    corrupt = buffer;
    WriteWord(corrupt, format::DOMAIN_SCHEMA_OFFSET, format::DOMAIN_SCHEMA_1 + 1);
    requireRejected(corrupt, bytes, FoliagePageStatus::UnsupportedVersion, FoliagePageIssue::DomainSchema);
    corrupt = buffer;
    WriteWord(corrupt, format::PLACEMENT_COUNT_OFFSET, format::MAX_PLACEMENTS + 1);
    requireRejected(corrupt, bytes, FoliagePageStatus::LimitExceeded, FoliagePageIssue::PlacementCount);
    corrupt = buffer;
    WriteWord(corrupt, format::PLACEMENT_COUNT_OFFSET, PLACEMENT_COUNT + 1);
    requireRejected(corrupt, bytes, FoliagePageStatus::Malformed, FoliagePageIssue::Size);

    requireRejected(buffer,
                    bytes,
                    FoliagePageStatus::RevisionMismatch,
                    FoliagePageIssue::DomainId,
                    { .DomainId = golden::DOMAIN_ID + 1, .TerrainRevision = golden::TERRAIN_REVISION });
    requireRejected(buffer,
                    bytes,
                    FoliagePageStatus::RevisionMismatch,
                    FoliagePageIssue::TerrainRevision,
                    { .DomainId = golden::DOMAIN_ID, .TerrainRevision = golden::TERRAIN_REVISION + 1 });

    const usize placements = GetPlacementTableOffset();
    corrupt = buffer;
    for (usize i = 0; i < format::PLACEMENT_RECORD_BYTES; ++i)
    {
        corrupt[placements + format::PLACEMENT_RECORD_BYTES + i] = buffer[placements + i];
    }
    requireRejected(corrupt, bytes, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementOrder);
    corrupt = buffer;
    WriteHalf(corrupt, placements + format::PLACEMENT_SPECIES_FIELD, SPECIES_COUNT);
    requireRejected(corrupt, bytes, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementSpecies);

    FoliagePageSetView view;
    FoliageLayer unchanged;
    CHECK(ReadFoliagePageSet(BorrowedView<const uint8>(nullptr, 1), golden::BINDING, {}, view, diagnostic) ==
          FoliagePageStatus::InvalidArgument);
    CHECK(view.TryGetLayer(0, unchanged) == FoliagePageStatus::InvalidHandle);
}

TEST_CASE("Every accepted single-byte corruption is canonical", "[foliage_pages]")
{
    // Any byte change the reader admits must re-encode to exactly the changed bytes, so equal
    // accepted page sets always have equal bytes and nothing outside the schema survives.
    Buffer buffer{};
    const usize bytes = EncodeGolden(buffer);
    constexpr uint8 FLIPS[] = {0x01, 0x80, 0xff};
    usize accepted = 0;
    for (usize i = 0; i < bytes; ++i)
    {
        for (const uint8 flip : FLIPS)
        {
            Buffer corrupt = buffer;
            corrupt[i] = static_cast<uint8>(corrupt[i] ^ flip);
            FoliagePageSetView view;
            FoliagePageDiagnostic diagnostic;
            const FoliagePageStatus status = Read(corrupt, bytes, view, diagnostic);
            REQUIRE(diagnostic.Status == status);
            if (status != FoliagePageStatus::Ok)
            {
                REQUIRE(diagnostic.Issue != FoliagePageIssue::None);
                REQUIRE(diagnostic.Offset <= bytes);
                continue;
            }
            ++accepted;
            Buffer reencoded{};
            REQUIRE(ReencodeView(view, reencoded) == bytes);
            for (usize j = 0; j < bytes; ++j)
            {
                REQUIRE(reencoded[j] == corrupt[j]);
            }
        }
    }
    // Free fields (yaw, digest, origin, species revisions) accept corruption; most fields do not.
    CHECK(accepted > 0);
    CHECK(accepted < bytes * sizeof(FLIPS));
}
