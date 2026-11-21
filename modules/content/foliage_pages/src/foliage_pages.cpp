#include <ludus/content/foliage_pages/foliage_pages.h>

#include <ludus/foundation/base/borrowed_view.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

// LUFOL follows the LUSTR precedent (modules/content/string_dictionary): a
// tightly packed little-endian blob with derived field offsets, validated in
// full before a zero-copy view is exposed. The encoder and reader share one
// validation pass so their rules cannot drift. Original implementation; see
// docs/architecture/foliage.md, "Cooked pages and publication".

namespace ludus::content::foliage_pages
{
namespace format = foliage_format;

namespace
{
/// Byte offsets of the four tables and the end of a page set, derived from its counts.
struct TableLayout final
{
    usize Layers = 0;
    usize Species = 0;
    usize Pages = 0;
    usize Placements = 0;
    usize End = 0;
};

/// Table counts of one page set.
struct TableCounts final
{
    usize Layers = 0;
    usize Species = 0;
    usize Pages = 0;
    usize Placements = 0;
};

/// @pre Every count is within the format limits, so no product or sum overflows.
TableLayout MakeTableLayout(const TableCounts& counts) noexcept
{
    TableLayout layout;
    layout.Layers = format::HEADER_BYTES;
    layout.Species = layout.Layers + counts.Layers * format::LAYER_RECORD_BYTES;
    layout.Pages = layout.Species + counts.Species * format::SPECIES_RECORD_BYTES;
    layout.Placements = layout.Pages + counts.Pages * format::PAGE_RECORD_BYTES;
    layout.End = layout.Placements + counts.Placements * format::PLACEMENT_RECORD_BYTES;
    return layout;
}

template <typename ValueType>
ValueType ReadField(const uint8* bytes, usize offset) noexcept
{
    ValueType value{};
    const foundation::BorrowedView<const uint8> field{bytes + offset, sizeof(ValueType)};
    [[maybe_unused]] const bool isRead = foundation::TryReadLittleEndian(field, value);
    LUDUS_ASSERT(isRead == true);
    return value;
}

template <typename ValueType>
void WriteField(uint8* bytes, usize offset, ValueType value) noexcept
{
    const foundation::BorrowedView<uint8> field{bytes + offset, sizeof(ValueType)};
    [[maybe_unused]] const bool isWritten = foundation::TryWriteLittleEndian(value, field);
    LUDUS_ASSERT(isWritten == true);
}

// Signed fields are two's complement; the conversions below are exact since C++20.
int16 ReadInt16(const uint8* bytes, usize offset) noexcept
{
    return static_cast<int16>(ReadField<uint16>(bytes, offset));
}

int32 ReadInt32(const uint8* bytes, usize offset) noexcept
{
    return static_cast<int32>(ReadField<uint32>(bytes, offset));
}

int64 ReadInt64(const uint8* bytes, usize offset) noexcept
{
    return static_cast<int64>(ReadField<uint64>(bytes, offset));
}

FoliageLayer DecodeLayer(const uint8* record) noexcept
{
    FoliageLayer layer;
    layer.Id = ReadField<uint16>(record, format::LAYER_ID_FIELD);
    layer.SlotsPerCell = ReadField<uint16>(record, format::LAYER_SLOTS_PER_CELL_FIELD);
    layer.CellSize = ReadField<uint32>(record, format::LAYER_CELL_SIZE_FIELD);
    return layer;
}

FoliageSpecies DecodeSpecies(const uint8* record) noexcept
{
    FoliageSpecies species;
    species.Key = ReadField<uint64>(record, format::SPECIES_KEY_FIELD);
    species.PlacementRevision = ReadField<uint64>(record, format::SPECIES_PLACEMENT_REVISION_FIELD);
    species.VariantCount = ReadField<uint32>(record, format::SPECIES_VARIANT_COUNT_FIELD);
    return species;
}

FoliagePage DecodePage(const uint8* record) noexcept
{
    FoliagePage page;
    page.PageX = ReadInt32(record, format::PAGE_X_FIELD);
    page.PageZ = ReadInt32(record, format::PAGE_Z_FIELD);
    page.FirstPlacement = ReadField<uint32>(record, format::PAGE_FIRST_PLACEMENT_FIELD);
    page.PlacementCount = ReadField<uint32>(record, format::PAGE_PLACEMENT_COUNT_FIELD);
    page.Bounds.MinimumX = ReadInt64(record, format::PAGE_MINIMUM_X_FIELD);
    page.Bounds.MinimumZ = ReadInt64(record, format::PAGE_MINIMUM_Z_FIELD);
    page.Bounds.MaximumX = ReadInt64(record, format::PAGE_MAXIMUM_X_FIELD);
    page.Bounds.MaximumZ = ReadInt64(record, format::PAGE_MAXIMUM_Z_FIELD);
    page.Bounds.MinimumHeight = ReadInt32(record, format::PAGE_MINIMUM_HEIGHT_FIELD);
    page.Bounds.MaximumHeight = ReadInt32(record, format::PAGE_MAXIMUM_HEIGHT_FIELD);
    return page;
}

FoliagePlacement DecodePlacement(const uint8* record) noexcept
{
    FoliagePlacement placement;
    placement.Layer = ReadField<uint16>(record, format::PLACEMENT_LAYER_FIELD);
    placement.Slot = ReadField<uint16>(record, format::PLACEMENT_SLOT_FIELD);
    placement.CellX = ReadInt32(record, format::PLACEMENT_CELL_X_FIELD);
    placement.CellZ = ReadInt32(record, format::PLACEMENT_CELL_Z_FIELD);
    placement.OffsetX = ReadField<uint16>(record, format::PLACEMENT_OFFSET_X_FIELD);
    placement.OffsetZ = ReadField<uint16>(record, format::PLACEMENT_OFFSET_Z_FIELD);
    placement.Height = ReadInt32(record, format::PLACEMENT_HEIGHT_FIELD);
    placement.Yaw = ReadField<uint16>(record, format::PLACEMENT_YAW_FIELD);
    placement.Scale = ReadField<uint16>(record, format::PLACEMENT_SCALE_FIELD);
    placement.NormalX = ReadInt16(record, format::PLACEMENT_NORMAL_X_FIELD);
    placement.NormalZ = ReadInt16(record, format::PLACEMENT_NORMAL_Z_FIELD);
    placement.Species = ReadField<uint16>(record, format::PLACEMENT_SPECIES_FIELD);
    placement.Variant = ReadField<uint16>(record, format::PLACEMENT_VARIANT_FIELD);
    return placement;
}

FoliagePageSetHeader DecodeHeader(const uint8* bytes) noexcept
{
    FoliagePageSetHeader header;
    header.GeneratorVersion = ReadField<uint32>(bytes, format::GENERATOR_VERSION_OFFSET);
    header.PageSize = ReadField<uint32>(bytes, format::PAGE_SIZE_OFFSET);
    header.DomainId = ReadField<uint64>(bytes, format::DOMAIN_ID_OFFSET);
    header.OriginX = ReadInt64(bytes, format::ORIGIN_X_OFFSET);
    header.OriginZ = ReadInt64(bytes, format::ORIGIN_Z_OFFSET);
    header.TerrainRevision = ReadField<uint64>(bytes, format::TERRAIN_REVISION_OFFSET);
    for (usize i = 0; i < format::INPUT_DIGEST_BYTES; ++i)
    {
        header.InputDigest[i] = bytes[format::INPUT_DIGEST_OFFSET + i];
    }
    return header;
}

void EncodeHeader(uint8* bytes, const FoliagePageSetHeader& header, const TableCounts& counts) noexcept
{
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        bytes[format::MAGIC_OFFSET + i] = static_cast<uint8>(format::MAGIC[i]);
    }
    WriteField<uint32>(bytes, format::FORMAT_VERSION_OFFSET, format::CURRENT_FORMAT_VERSION);
    WriteField<uint32>(bytes, format::GENERATOR_VERSION_OFFSET, header.GeneratorVersion);
    WriteField<uint32>(bytes, format::DOMAIN_SCHEMA_OFFSET, format::DOMAIN_SCHEMA_1);
    WriteField<uint32>(bytes, format::PAGE_SIZE_OFFSET, header.PageSize);
    WriteField<uint64>(bytes, format::DOMAIN_ID_OFFSET, header.DomainId);
    WriteField<uint64>(bytes, format::ORIGIN_X_OFFSET, static_cast<uint64>(header.OriginX));
    WriteField<uint64>(bytes, format::ORIGIN_Z_OFFSET, static_cast<uint64>(header.OriginZ));
    WriteField<uint64>(bytes, format::TERRAIN_REVISION_OFFSET, header.TerrainRevision);
    // Counts were checked against the format limits, so they fit uint32.
    WriteField<uint32>(bytes, format::LAYER_COUNT_OFFSET, static_cast<uint32>(counts.Layers));
    WriteField<uint32>(bytes, format::SPECIES_COUNT_OFFSET, static_cast<uint32>(counts.Species));
    WriteField<uint32>(bytes, format::PAGE_COUNT_OFFSET, static_cast<uint32>(counts.Pages));
    WriteField<uint32>(bytes, format::PLACEMENT_COUNT_OFFSET, static_cast<uint32>(counts.Placements));
    for (usize i = 0; i < format::INPUT_DIGEST_BYTES; ++i)
    {
        bytes[format::INPUT_DIGEST_OFFSET + i] = header.InputDigest[i];
    }
}

void EncodeLayer(uint8* record, const FoliageLayer& layer) noexcept
{
    WriteField<uint16>(record, format::LAYER_ID_FIELD, layer.Id);
    WriteField<uint16>(record, format::LAYER_SLOTS_PER_CELL_FIELD, layer.SlotsPerCell);
    WriteField<uint32>(record, format::LAYER_CELL_SIZE_FIELD, layer.CellSize);
}

void EncodeSpecies(uint8* record, const FoliageSpecies& species) noexcept
{
    WriteField<uint64>(record, format::SPECIES_KEY_FIELD, species.Key);
    WriteField<uint64>(record, format::SPECIES_PLACEMENT_REVISION_FIELD, species.PlacementRevision);
    WriteField<uint32>(record, format::SPECIES_VARIANT_COUNT_FIELD, species.VariantCount);
}

void EncodePage(uint8* record, const FoliagePage& page) noexcept
{
    WriteField<uint32>(record, format::PAGE_X_FIELD, static_cast<uint32>(page.PageX));
    WriteField<uint32>(record, format::PAGE_Z_FIELD, static_cast<uint32>(page.PageZ));
    WriteField<uint32>(record, format::PAGE_FIRST_PLACEMENT_FIELD, page.FirstPlacement);
    WriteField<uint32>(record, format::PAGE_PLACEMENT_COUNT_FIELD, page.PlacementCount);
    WriteField<uint64>(record, format::PAGE_MINIMUM_X_FIELD, static_cast<uint64>(page.Bounds.MinimumX));
    WriteField<uint64>(record, format::PAGE_MINIMUM_Z_FIELD, static_cast<uint64>(page.Bounds.MinimumZ));
    WriteField<uint64>(record, format::PAGE_MAXIMUM_X_FIELD, static_cast<uint64>(page.Bounds.MaximumX));
    WriteField<uint64>(record, format::PAGE_MAXIMUM_Z_FIELD, static_cast<uint64>(page.Bounds.MaximumZ));
    WriteField<uint32>(record, format::PAGE_MINIMUM_HEIGHT_FIELD, static_cast<uint32>(page.Bounds.MinimumHeight));
    WriteField<uint32>(record, format::PAGE_MAXIMUM_HEIGHT_FIELD, static_cast<uint32>(page.Bounds.MaximumHeight));
}

void EncodePlacement(uint8* record, const FoliagePlacement& placement) noexcept
{
    WriteField<uint16>(record, format::PLACEMENT_LAYER_FIELD, placement.Layer);
    WriteField<uint16>(record, format::PLACEMENT_SLOT_FIELD, placement.Slot);
    WriteField<uint32>(record, format::PLACEMENT_CELL_X_FIELD, static_cast<uint32>(placement.CellX));
    WriteField<uint32>(record, format::PLACEMENT_CELL_Z_FIELD, static_cast<uint32>(placement.CellZ));
    WriteField<uint16>(record, format::PLACEMENT_OFFSET_X_FIELD, placement.OffsetX);
    WriteField<uint16>(record, format::PLACEMENT_OFFSET_Z_FIELD, placement.OffsetZ);
    WriteField<uint32>(record, format::PLACEMENT_HEIGHT_FIELD, static_cast<uint32>(placement.Height));
    WriteField<uint16>(record, format::PLACEMENT_YAW_FIELD, placement.Yaw);
    WriteField<uint16>(record, format::PLACEMENT_SCALE_FIELD, placement.Scale);
    WriteField<uint16>(record, format::PLACEMENT_NORMAL_X_FIELD, static_cast<uint16>(placement.NormalX));
    WriteField<uint16>(record, format::PLACEMENT_NORMAL_Z_FIELD, static_cast<uint16>(placement.NormalZ));
    WriteField<uint16>(record, format::PLACEMENT_SPECIES_FIELD, placement.Species);
    WriteField<uint16>(record, format::PLACEMENT_VARIANT_FIELD, placement.Variant);
}

FoliagePageStatus Fail(FoliagePageDiagnostic& outDiagnostic,
                       FoliagePageStatus status,
                       FoliagePageIssue issue,
                       usize index,
                       usize offset) noexcept
{
    outDiagnostic = { .Status = status, .Issue = issue, .Index = index, .Offset = offset };
    return status;
}

/// @brief Tables under validation: either an encoder description or validated-size reader bytes.
///
/// Byte offsets are reported only for reader bytes; description failures report zero.
class PageSetSource final
{
public:
    static PageSetSource FromDescription(const FoliagePageSetDescription& description) noexcept
    {
        PageSetSource source;
        source.mDescriptionOrNull = &description;
        source.mCounts =
        {
            .Layers = description.Layers.GetSize(),
            .Species = description.Species.GetSize(),
            .Pages = description.Pages.GetSize(),
            .Placements = description.Placements.GetSize(),
        };
        return source;
    }

    /// @pre bytes holds the complete page set described by counts.
    static PageSetSource FromBytes(const uint8* bytes, const TableCounts& counts) noexcept
    {
        PageSetSource source;
        source.mBytesOrNull = bytes;
        source.mCounts = counts;
        source.mLayout = MakeTableLayout(counts);
        return source;
    }

    [[nodiscard]] const TableCounts& GetCounts() const noexcept
    {
        return mCounts;
    }

    [[nodiscard]] FoliageLayer GetLayer(usize index) const noexcept
    {
        if (mDescriptionOrNull != nullptr)
        {
            return mDescriptionOrNull->Layers[index];
        }
        return DecodeLayer(mBytesOrNull + GetLayerOffset(index));
    }

    [[nodiscard]] FoliageSpecies GetSpecies(usize index) const noexcept
    {
        if (mDescriptionOrNull != nullptr)
        {
            return mDescriptionOrNull->Species[index];
        }
        return DecodeSpecies(mBytesOrNull + GetSpeciesOffset(index));
    }

    [[nodiscard]] FoliagePage GetPage(usize index) const noexcept
    {
        if (mDescriptionOrNull != nullptr)
        {
            return mDescriptionOrNull->Pages[index];
        }
        return DecodePage(mBytesOrNull + GetPageOffset(index));
    }

    [[nodiscard]] FoliagePlacement GetPlacement(usize index) const noexcept
    {
        if (mDescriptionOrNull != nullptr)
        {
            return mDescriptionOrNull->Placements[index];
        }
        return DecodePlacement(mBytesOrNull + GetPlacementOffset(index));
    }

    [[nodiscard]] usize GetLayerOffset(usize index) const noexcept
    {
        return mBytesOrNull == nullptr ? 0 : mLayout.Layers + index * format::LAYER_RECORD_BYTES;
    }

    [[nodiscard]] usize GetSpeciesOffset(usize index) const noexcept
    {
        return mBytesOrNull == nullptr ? 0 : mLayout.Species + index * format::SPECIES_RECORD_BYTES;
    }

    [[nodiscard]] usize GetPageOffset(usize index) const noexcept
    {
        return mBytesOrNull == nullptr ? 0 : mLayout.Pages + index * format::PAGE_RECORD_BYTES;
    }

    [[nodiscard]] usize GetPlacementOffset(usize index) const noexcept
    {
        return mBytesOrNull == nullptr ? 0 : mLayout.Placements + index * format::PLACEMENT_RECORD_BYTES;
    }

    [[nodiscard]] usize GetHeaderFieldOffset(usize offset) const noexcept
    {
        return mBytesOrNull == nullptr ? 0 : offset;
    }

private:
    const FoliagePageSetDescription* mDescriptionOrNull = nullptr;
    const uint8* mBytesOrNull = nullptr;
    TableCounts mCounts;
    TableLayout mLayout;
};

bool AreLimitsValid(const FoliagePageLimits& limits) noexcept
{
    return limits.MaximumBytes <= format::MAX_PAGE_SET_BYTES && limits.MaximumLayers <= format::MAX_LAYERS &&
           limits.MaximumSpecies <= format::MAX_SPECIES && limits.MaximumPages <= format::MAX_PAGES &&
           limits.MaximumPlacements <= format::MAX_PLACEMENTS &&
           limits.MaximumPlacementsPerPage <= format::MAX_PLACEMENTS_PER_PAGE;
}

/// Count admission, checked before any table offset is derived from the counts.
FoliagePageStatus ValidateCounts(const PageSetSource& source,
                                 const FoliagePageLimits& limits,
                                 FoliagePageDiagnostic& outDiagnostic) noexcept
{
    const TableCounts& counts = source.GetCounts();
    if (counts.Layers > limits.MaximumLayers)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::LimitExceeded,
                    FoliagePageIssue::LayerCount,
                    0,
                    source.GetHeaderFieldOffset(format::LAYER_COUNT_OFFSET));
    }
    if (counts.Species > limits.MaximumSpecies)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::LimitExceeded,
                    FoliagePageIssue::SpeciesCount,
                    0,
                    source.GetHeaderFieldOffset(format::SPECIES_COUNT_OFFSET));
    }
    if (counts.Pages > limits.MaximumPages)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::LimitExceeded,
                    FoliagePageIssue::PageCount,
                    0,
                    source.GetHeaderFieldOffset(format::PAGE_COUNT_OFFSET));
    }
    if (counts.Placements > limits.MaximumPlacements)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::LimitExceeded,
                    FoliagePageIssue::PlacementCount,
                    0,
                    source.GetHeaderFieldOffset(format::PLACEMENT_COUNT_OFFSET));
    }
    return FoliagePageStatus::Ok;
}

FoliagePageStatus ValidateHeader(const PageSetSource& source,
                                 const FoliagePageSetHeader& header,
                                 FoliagePageDiagnostic& outDiagnostic) noexcept
{
    if (header.GeneratorVersion == 0)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::Malformed,
                    FoliagePageIssue::GeneratorVersion,
                    0,
                    source.GetHeaderFieldOffset(format::GENERATOR_VERSION_OFFSET));
    }
    if (header.DomainId == 0)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::Malformed,
                    FoliagePageIssue::DomainId,
                    0,
                    source.GetHeaderFieldOffset(format::DOMAIN_ID_OFFSET));
    }
    if (header.PageSize == 0 || header.PageSize > format::MAX_PAGE_SIZE)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::Malformed,
                    FoliagePageIssue::PageSize,
                    0,
                    source.GetHeaderFieldOffset(format::PAGE_SIZE_OFFSET));
    }
    return FoliagePageStatus::Ok;
}

/// Layers are few (MAX_LAYERS); they are decoded once and searched by ID.
using LayerTable = foundation::StaticArray<FoliageLayer, format::MAX_LAYERS>;

FoliagePageStatus ValidateLayers(const PageSetSource& source,
                                 uint32 pageSize,
                                 LayerTable& outLayers,
                                 FoliagePageDiagnostic& outDiagnostic) noexcept
{
    const usize layerCount = source.GetCounts().Layers;
    for (usize i = 0; i < layerCount; ++i)
    {
        const FoliageLayer layer = source.GetLayer(i);
        if (i != 0 && layer.Id <= outLayers[i - 1].Id)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::LayerOrder,
                        i,
                        source.GetLayerOffset(i));
        }
        if (layer.SlotsPerCell == 0 || layer.SlotsPerCell > format::MAX_SLOTS_PER_CELL)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::LayerSlots,
                        i,
                        source.GetLayerOffset(i));
        }
        if (layer.CellSize == 0 || layer.CellSize > format::MAX_CELL_SIZE || pageSize % layer.CellSize != 0)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::LayerCellSize,
                        i,
                        source.GetLayerOffset(i));
        }
        outLayers[i] = layer;
    }
    return FoliagePageStatus::Ok;
}

FoliagePageStatus ValidateSpecies(const PageSetSource& source, FoliagePageDiagnostic& outDiagnostic) noexcept
{
    const usize speciesCount = source.GetCounts().Species;
    uint64 previousKey = 0;
    for (usize i = 0; i < speciesCount; ++i)
    {
        const FoliageSpecies species = source.GetSpecies(i);
        // Zero is reserved, so a strictly increasing sequence starting above zero rejects both.
        if (species.Key <= previousKey)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::SpeciesKey,
                        i,
                        source.GetSpeciesOffset(i));
        }
        if (species.VariantCount == 0 || species.VariantCount > format::MAX_VARIANTS_PER_SPECIES)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::SpeciesVariants,
                        i,
                        source.GetSpeciesOffset(i));
        }
        previousKey = species.Key;
    }
    return FoliagePageStatus::Ok;
}

bool IsPageAfter(const FoliagePage& page, const FoliagePage& previous) noexcept
{
    return page.PageZ > previous.PageZ || (page.PageZ == previous.PageZ && page.PageX > previous.PageX);
}

bool IsPlacementAfter(const FoliagePlacement& placement, const FoliagePlacement& previous) noexcept
{
    if (placement.Layer != previous.Layer)
    {
        return placement.Layer > previous.Layer;
    }
    if (placement.CellZ != previous.CellZ)
    {
        return placement.CellZ > previous.CellZ;
    }
    if (placement.CellX != previous.CellX)
    {
        return placement.CellX > previous.CellX;
    }
    return placement.Slot > previous.Slot;
}

bool AreBoundsOrdered(const FoliagePageBounds& bounds) noexcept
{
    return bounds.MinimumX <= bounds.MaximumX && bounds.MinimumZ <= bounds.MaximumZ &&
           bounds.MinimumHeight <= bounds.MaximumHeight;
}

bool IsRootInPage(const FoliageRoot& root, const FoliagePage& page, uint32 pageSize) noexcept
{
    // |page| <= 2^31 and pageSize <= 2^24 keep both ends of the half-open extent far below 2^63.
    const int64 size = pageSize;
    const int64 minimumX = int64{page.PageX} * size;
    const int64 minimumZ = int64{page.PageZ} * size;
    return root.X >= minimumX && root.X < minimumX + size && root.Z >= minimumZ && root.Z < minimumZ + size;
}

bool FindLayer(const LayerTable& layers, usize layerCount, uint16 id, usize& outIndex) noexcept
{
    usize low = 0;
    usize high = layerCount;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (layers[middle].Id < id)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == layerCount || layers[low].Id != id)
    {
        return false;
    }
    outIndex = low;
    return true;
}

FoliagePageStatus ValidatePlacement(const PageSetSource& source,
                                    usize index,
                                    const LayerTable& layers,
                                    const FoliagePage& page,
                                    uint32 pageSize,
                                    FoliagePageDiagnostic& outDiagnostic) noexcept
{
    const FoliagePlacement placement = source.GetPlacement(index);
    const usize offset = source.GetPlacementOffset(index);
    usize layerIndex = 0;
    if (FindLayer(layers, source.GetCounts().Layers, placement.Layer, layerIndex) == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementLayer, index, offset);
    }
    const FoliageLayer& layer = layers[layerIndex];
    if (placement.Slot >= layer.SlotsPerCell)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementSlot, index, offset);
    }
    if (placement.OffsetX >= layer.CellSize || placement.OffsetZ >= layer.CellSize)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementOffset, index, offset);
    }
    const FoliageRoot root = ComputeFoliageRoot(layer.CellSize, placement);
    if (IsRootInPage(root, page, pageSize) == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementPage, index, offset);
    }
    if (index != page.FirstPlacement && IsPlacementAfter(placement, source.GetPlacement(index - 1)) == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementOrder, index, offset);
    }
    if (placement.Scale == 0)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementScale, index, offset);
    }
    const int64 normalX = placement.NormalX;
    const int64 normalZ = placement.NormalZ;
    const int64 unit = format::NORMAL_UNITS;
    if (normalX * normalX + normalZ * normalZ > unit * unit)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementNormal, index, offset);
    }
    if (placement.Species >= source.GetCounts().Species)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementSpecies, index, offset);
    }
    if (placement.Variant >= source.GetSpecies(placement.Species).VariantCount)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementVariant, index, offset);
    }
    const FoliagePageBounds& bounds = page.Bounds;
    if (root.X < bounds.MinimumX || root.X > bounds.MaximumX || root.Z < bounds.MinimumZ || root.Z > bounds.MaximumZ ||
        placement.Height < bounds.MinimumHeight || placement.Height > bounds.MaximumHeight)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PlacementBounds, index, offset);
    }
    return FoliagePageStatus::Ok;
}

FoliagePageStatus ValidatePages(const PageSetSource& source,
                                const LayerTable& layers,
                                uint32 pageSize,
                                const FoliagePageLimits& limits,
                                FoliagePageDiagnostic& outDiagnostic) noexcept
{
    const TableCounts& counts = source.GetCounts();
    usize nextPlacement = 0;
    FoliagePage previous;
    for (usize i = 0; i < counts.Pages; ++i)
    {
        const FoliagePage page = source.GetPage(i);
        const usize offset = source.GetPageOffset(i);
        if (i != 0 && IsPageAfter(page, previous) == false)
        {
            return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PageOrder, i, offset);
        }
        if (page.FirstPlacement != nextPlacement)
        {
            return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PageRange, i, offset);
        }
        if (page.PlacementCount > limits.MaximumPlacementsPerPage)
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::LimitExceeded,
                        FoliagePageIssue::PagePlacementCount,
                        i,
                        offset);
        }
        // Both terms are at most MAX_PLACEMENTS, far below the usize range.
        const usize end = nextPlacement + page.PlacementCount;
        if (end > counts.Placements)
        {
            return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PageRange, i, offset);
        }
        const bool isEmpty = page.PlacementCount == 0;
        if ((isEmpty == true && (page.Bounds == FoliagePageBounds{}) == false) ||
            (isEmpty == false && AreBoundsOrdered(page.Bounds) == false))
        {
            return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::PageBounds, i, offset);
        }
        for (usize placement = nextPlacement; placement < end; ++placement)
        {
            const FoliagePageStatus status =
                ValidatePlacement(source, placement, layers, page, pageSize, outDiagnostic);
            if (status != FoliagePageStatus::Ok)
            {
                return status;
            }
        }
        nextPlacement = end;
        previous = page;
    }
    if (nextPlacement != counts.Placements)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::Malformed,
                    FoliagePageIssue::PlacementCount,
                    0,
                    source.GetHeaderFieldOffset(format::PLACEMENT_COUNT_OFFSET));
    }
    return FoliagePageStatus::Ok;
}

/// Table rules shared by encoding and reading; counts were admitted and the header validated.
FoliagePageStatus ValidateTables(const PageSetSource& source,
                                 const FoliagePageSetHeader& header,
                                 const FoliagePageLimits& limits,
                                 FoliagePageDiagnostic& outDiagnostic) noexcept
{
    LayerTable layers{};
    FoliagePageStatus status = ValidateLayers(source, header.PageSize, layers, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    status = ValidateSpecies(source, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    return ValidatePages(source, layers, header.PageSize, limits, outDiagnostic);
}

FoliagePageStatus ValidateDescription(const FoliagePageSetDescription& description,
                                      const FoliagePageLimits& limits,
                                      usize& outBytes,
                                      FoliagePageDiagnostic& outDiagnostic) noexcept
{
    if (AreLimitsValid(limits) == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::InvalidArgument, FoliagePageIssue::Limits, 0, 0);
    }
    if (description.Layers.IsValid() == false || description.Species.IsValid() == false ||
        description.Pages.IsValid() == false || description.Placements.IsValid() == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::InvalidArgument, FoliagePageIssue::InputView, 0, 0);
    }
    const PageSetSource source = PageSetSource::FromDescription(description);
    FoliagePageStatus status = ValidateCounts(source, limits, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    const usize bytes = MakeTableLayout(source.GetCounts()).End;
    if (bytes > limits.MaximumBytes)
    {
        return Fail(outDiagnostic, FoliagePageStatus::LimitExceeded, FoliagePageIssue::Size, 0, 0);
    }
    status = ValidateHeader(source, description.Header, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    status = ValidateTables(source, description.Header, limits, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    outBytes = bytes;
    return FoliagePageStatus::Ok;
}

FoliagePageStatus ValidateBytesHeader(foundation::BorrowedView<const uint8> bytes,
                                      const FoliagePageLimits& limits,
                                      TableCounts& outCounts,
                                      FoliagePageDiagnostic& outDiagnostic) noexcept
{
    if (AreLimitsValid(limits) == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::InvalidArgument, FoliagePageIssue::Limits, 0, 0);
    }
    if (bytes.IsValid() == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::InvalidArgument, FoliagePageIssue::InputView, 0, 0);
    }
    const usize size = bytes.GetSize();
    if (size > limits.MaximumBytes)
    {
        return Fail(outDiagnostic, FoliagePageStatus::LimitExceeded, FoliagePageIssue::Size, 0, limits.MaximumBytes);
    }
    if (size < format::HEADER_BYTES)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::Size, 0, size);
    }
    const uint8* data = bytes.GetDataOrNull();
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        if (data[format::MAGIC_OFFSET + i] != static_cast<uint8>(format::MAGIC[i]))
        {
            return Fail(outDiagnostic,
                        FoliagePageStatus::Malformed,
                        FoliagePageIssue::Magic,
                        0,
                        format::MAGIC_OFFSET + i);
        }
    }
    if (ReadField<uint32>(data, format::FORMAT_VERSION_OFFSET) != format::FORMAT_VERSION_1)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::UnsupportedVersion,
                    FoliagePageIssue::FormatVersion,
                    0,
                    format::FORMAT_VERSION_OFFSET);
    }
    if (ReadField<uint32>(data, format::DOMAIN_SCHEMA_OFFSET) != format::DOMAIN_SCHEMA_1)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::UnsupportedVersion,
                    FoliagePageIssue::DomainSchema,
                    0,
                    format::DOMAIN_SCHEMA_OFFSET);
    }
    const TableCounts counts =
    {
        .Layers = ReadField<uint32>(data, format::LAYER_COUNT_OFFSET),
        .Species = ReadField<uint32>(data, format::SPECIES_COUNT_OFFSET),
        .Pages = ReadField<uint32>(data, format::PAGE_COUNT_OFFSET),
        .Placements = ReadField<uint32>(data, format::PLACEMENT_COUNT_OFFSET),
    };
    const FoliagePageStatus status = ValidateCounts(PageSetSource::FromBytes(data, counts), limits, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    const usize end = MakeTableLayout(counts).End;
    if (end != size)
    {
        return Fail(outDiagnostic, FoliagePageStatus::Malformed, FoliagePageIssue::Size, 0, end < size ? end : size);
    }
    outCounts = counts;
    return FoliagePageStatus::Ok;
}
} // namespace

FoliagePageSetHeader FoliagePageSetView::GetHeader() const noexcept
{
    if (mBytesOrNull == nullptr)
    {
        return {};
    }
    return DecodeHeader(mBytesOrNull);
}

FoliagePageStatus FoliagePageSetView::TryGetLayer(usize index, FoliageLayer& outLayer) const noexcept
{
    if (mBytesOrNull == nullptr || index >= mLayerCount)
    {
        return FoliagePageStatus::InvalidHandle;
    }
    outLayer = DecodeLayer(mBytesOrNull + format::HEADER_BYTES + index * format::LAYER_RECORD_BYTES);
    return FoliagePageStatus::Ok;
}

FoliagePageStatus FoliagePageSetView::TryGetSpecies(usize index, FoliageSpecies& outSpecies) const noexcept
{
    if (mBytesOrNull == nullptr || index >= mSpeciesCount)
    {
        return FoliagePageStatus::InvalidHandle;
    }
    const TableLayout layout = MakeTableLayout({ .Layers = mLayerCount });
    outSpecies = DecodeSpecies(mBytesOrNull + layout.Species + index * format::SPECIES_RECORD_BYTES);
    return FoliagePageStatus::Ok;
}

FoliagePageStatus FoliagePageSetView::TryGetPage(usize index, FoliagePage& outPage) const noexcept
{
    if (mBytesOrNull == nullptr || index >= mPageCount)
    {
        return FoliagePageStatus::InvalidHandle;
    }
    const TableLayout layout = MakeTableLayout({ .Layers = mLayerCount, .Species = mSpeciesCount });
    outPage = DecodePage(mBytesOrNull + layout.Pages + index * format::PAGE_RECORD_BYTES);
    return FoliagePageStatus::Ok;
}

FoliagePageStatus FoliagePageSetView::TryGetPlacement(usize index, FoliagePlacement& outPlacement) const noexcept
{
    if (mBytesOrNull == nullptr || index >= mPlacementCount)
    {
        return FoliagePageStatus::InvalidHandle;
    }
    const TableLayout layout = MakeTableLayout(
    {
        .Layers = mLayerCount,
        .Species = mSpeciesCount,
        .Pages = mPageCount,
    });
    outPlacement = DecodePlacement(mBytesOrNull + layout.Placements + index * format::PLACEMENT_RECORD_BYTES);
    return FoliagePageStatus::Ok;
}

FoliagePageStatus FoliagePageSetView::TryFindPage(int32 pageX, int32 pageZ, usize& outIndex) const noexcept
{
    if (mBytesOrNull == nullptr)
    {
        return FoliagePageStatus::InvalidHandle;
    }
    const TableLayout layout = MakeTableLayout({ .Layers = mLayerCount, .Species = mSpeciesCount });
    const FoliagePage target = { .PageX = pageX, .PageZ = pageZ };
    usize low = 0;
    usize high = mPageCount;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        const FoliagePage page = DecodePage(mBytesOrNull + layout.Pages + middle * format::PAGE_RECORD_BYTES);
        if (IsPageAfter(target, page) == true)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == mPageCount)
    {
        return FoliagePageStatus::NotFound;
    }
    const FoliagePage found = DecodePage(mBytesOrNull + layout.Pages + low * format::PAGE_RECORD_BYTES);
    if (found.PageX != pageX || found.PageZ != pageZ)
    {
        return FoliagePageStatus::NotFound;
    }
    outIndex = low;
    return FoliagePageStatus::Ok;
}

FoliagePageStatus GetFoliagePageSetBytes(const FoliagePageSetDescription& description,
                                         const FoliagePageLimits& limits,
                                         usize& outBytes,
                                         FoliagePageDiagnostic& outDiagnostic) noexcept
{
    usize bytes = 0;
    const FoliagePageStatus status = ValidateDescription(description, limits, bytes, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    outBytes = bytes;
    outDiagnostic = {};
    return FoliagePageStatus::Ok;
}

FoliagePageStatus WriteFoliagePageSet(const FoliagePageSetDescription& description,
                                      const FoliagePageLimits& limits,
                                      foundation::BorrowedView<uint8> outBytes,
                                      usize& outWritten,
                                      FoliagePageDiagnostic& outDiagnostic) noexcept
{
    if (outBytes.IsValid() == false)
    {
        return Fail(outDiagnostic, FoliagePageStatus::InvalidArgument, FoliagePageIssue::OutputView, 0, 0);
    }
    usize bytes = 0;
    const FoliagePageStatus status = ValidateDescription(description, limits, bytes, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    if (outBytes.GetSize() < bytes)
    {
        return Fail(outDiagnostic, FoliagePageStatus::CapacityExceeded, FoliagePageIssue::OutputView, 0, bytes);
    }
    const TableCounts counts =
    {
        .Layers = description.Layers.GetSize(),
        .Species = description.Species.GetSize(),
        .Pages = description.Pages.GetSize(),
        .Placements = description.Placements.GetSize(),
    };
    const TableLayout layout = MakeTableLayout(counts);
    uint8* data = outBytes.GetDataOrNull();
    EncodeHeader(data, description.Header, counts);
    for (usize i = 0; i < counts.Layers; ++i)
    {
        EncodeLayer(data + layout.Layers + i * format::LAYER_RECORD_BYTES, description.Layers[i]);
    }
    for (usize i = 0; i < counts.Species; ++i)
    {
        EncodeSpecies(data + layout.Species + i * format::SPECIES_RECORD_BYTES, description.Species[i]);
    }
    for (usize i = 0; i < counts.Pages; ++i)
    {
        EncodePage(data + layout.Pages + i * format::PAGE_RECORD_BYTES, description.Pages[i]);
    }
    for (usize i = 0; i < counts.Placements; ++i)
    {
        EncodePlacement(data + layout.Placements + i * format::PLACEMENT_RECORD_BYTES, description.Placements[i]);
    }
    outWritten = bytes;
    outDiagnostic = {};
    return FoliagePageStatus::Ok;
}

FoliagePageStatus ReadFoliagePageSet(foundation::BorrowedView<const uint8> bytes,
                                     const FoliagePageBinding& binding,
                                     const FoliagePageLimits& limits,
                                     FoliagePageSetView& outView,
                                     FoliagePageDiagnostic& outDiagnostic) noexcept
{
    TableCounts counts;
    FoliagePageStatus status = ValidateBytesHeader(bytes, limits, counts, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    const uint8* data = bytes.GetDataOrNull();
    const PageSetSource source = PageSetSource::FromBytes(data, counts);
    const FoliagePageSetHeader header = DecodeHeader(data);
    status = ValidateHeader(source, header, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    if (header.DomainId != binding.DomainId)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::RevisionMismatch,
                    FoliagePageIssue::DomainId,
                    0,
                    format::DOMAIN_ID_OFFSET);
    }
    if (header.TerrainRevision != binding.TerrainRevision)
    {
        return Fail(outDiagnostic,
                    FoliagePageStatus::RevisionMismatch,
                    FoliagePageIssue::TerrainRevision,
                    0,
                    format::TERRAIN_REVISION_OFFSET);
    }
    status = ValidateTables(source, header, limits, outDiagnostic);
    if (status != FoliagePageStatus::Ok)
    {
        return status;
    }
    FoliagePageSetView view;
    view.mBytesOrNull = data;
    // Admitted counts are at most the format limits, so they fit uint32.
    view.mLayerCount = static_cast<uint32>(counts.Layers);
    view.mSpeciesCount = static_cast<uint32>(counts.Species);
    view.mPageCount = static_cast<uint32>(counts.Pages);
    view.mPlacementCount = static_cast<uint32>(counts.Placements);
    outView = view;
    outDiagnostic = {};
    return FoliagePageStatus::Ok;
}
} // namespace ludus::content::foliage_pages
