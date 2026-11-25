#include <ludus/content/foliage_cook/foliage_cook.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/sort.hpp>

#include <ludus/content/content.h>

#include "internal/foliage_cook_validation.h"

#include <cstring>
#include <span>

// Canonical page set assembly and the placement-affecting input digest. Pages
// are ordered by address here, so the cook's page order never reaches a byte.
// Original implementation.

namespace ludus::content::foliage_cook
{
namespace
{
namespace format = foliage_format;
using foliage_pages::FoliageInputDigest;
using foliage_pages::FoliageLayer;
using foliage_pages::FoliagePage;
using foliage_pages::FoliagePlacement;
using foliage_pages::FoliageSpecies;
using foundation::Array;
using foundation::BorrowedView;

/// Bytes of the digest tag.
constexpr usize DIGEST_TAG_BYTES = 8;
/// Distinguishes this digest's canonical bytes from every other SHA-256 input, including the trailing NUL.
constexpr std::string_view DIGEST_TAG{"LUFOLCK\0", DIGEST_TAG_BYTES};
/// Bytes buffered before each hash update.
constexpr usize DIGEST_BUFFER_BYTES = 256;
constexpr usize UINT16_BYTES = 2;
constexpr usize UINT32_BYTES = 4;
constexpr usize UINT64_BYTES = 8;
constexpr usize BITS_PER_BYTE = 8;

/// Streams little-endian fields into SHA-256 through a small buffer.
class DigestWriter final
{
public:
    void WriteBytes(BorrowedView<const uint8> bytes) noexcept
    {
        for (const uint8 value : bytes)
        {
            WriteUInt8(value);
        }
    }
    void WriteUInt8(uint8 value) noexcept
    {
        if (mCount == DIGEST_BUFFER_BYTES)
        {
            flush();
        }
        mBuffer[mCount] = value;
        ++mCount;
    }
    void WriteUInt16(uint16 value) noexcept
    {
        writeLittleEndian<UINT16_BYTES>(value);
    }
    void WriteUInt32(uint32 value) noexcept
    {
        writeLittleEndian<UINT32_BYTES>(value);
    }
    void WriteUInt64(uint64 value) noexcept
    {
        writeLittleEndian<UINT64_BYTES>(value);
    }
    void WriteInt64(int64 value) noexcept
    {
        WriteUInt64(static_cast<uint64>(value));
    }
    void WriteBool(bool isSet) noexcept
    {
        WriteUInt8(isSet == true ? 1 : 0);
    }
    void WriteFloat32(foundation::float32 value) noexcept
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteUInt32(bits);
    }
    [[nodiscard]] bool TryFinish(FoliageInputDigest& outDigest) noexcept
    {
        flush();
        if (mHasFailed == true)
        {
            return false;
        }
        const content::Digest digest = mHasher.Finish();
        for (usize i = 0; i < outDigest.GetSize(); ++i)
        {
            outDigest[i] = digest.Data[i];
        }
        return true;
    }

private:
    template <usize BYTE_COUNT>
    void writeLittleEndian(uint64 value) noexcept
    {
        for (usize i = 0; i < BYTE_COUNT; ++i)
        {
            WriteUInt8(static_cast<uint8>(value >> (i * BITS_PER_BYTE)));
        }
    }
    void flush() noexcept
    {
        if (mCount > 0 && mHasher.Add(std::span<const uint8>{mBuffer.GetData(), mCount}) == false)
        {
            mHasFailed = true;
        }
        mCount = 0;
    }

    content::Hasher mHasher;
    foundation::StaticArray<uint8, DIGEST_BUFFER_BYTES> mBuffer{};
    usize mCount = 0;
    bool mHasFailed = false;
};

void WriteRange(DigestWriter& inoutWriter, const FoliageRange& range) noexcept
{
    inoutWriter.WriteBool(range.IsEnabled);
    inoutWriter.WriteInt64(range.Minimum);
    inoutWriter.WriteInt64(range.Maximum);
    inoutWriter.WriteUInt32(range.Feather);
}

void WriteChannel(DigestWriter& inoutWriter, BorrowedView<const uint16> values) noexcept
{
    inoutWriter.WriteUInt64(values.GetSize());
    for (const uint16 value : values)
    {
        inoutWriter.WriteUInt16(value);
    }
}

void WriteTerrain(DigestWriter& inoutWriter, const FoliageTerrainInput& terrain) noexcept
{
    const FoliageTerrainGrid& grid = terrain.Grid;
    const terrain::TerrainTileDescription description = grid.Tiles[0].GetDescription();
    inoutWriter.WriteUInt32(terrain::TERRAIN_TILE_HEIGHT_LAYOUT_VERSION);
    inoutWriter.WriteInt64(grid.MinimumTile.X);
    inoutWriter.WriteInt64(grid.MinimumTile.Z);
    inoutWriter.WriteUInt32(grid.TileColumns);
    inoutWriter.WriteUInt32(grid.TileRows);
    inoutWriter.WriteUInt64(description.Revision);
    inoutWriter.WriteUInt32(description.CellsPerSide);
    inoutWriter.WriteFloat32(description.SpacingMetres);
    inoutWriter.WriteUInt8(static_cast<uint8>(description.Codec.Encoding));
    inoutWriter.WriteFloat32(description.Codec.OriginMetres);
    inoutWriter.WriteFloat32(description.Codec.QuantumMetres);
    for (const terrain::TerrainTile& tile : grid.Tiles)
    {
        inoutWriter.WriteBytes(tile.GetEncodedHeights());
    }
    inoutWriter.WriteUInt64(terrain.Holes.GetSize());
    inoutWriter.WriteBytes(terrain.Holes);
    WriteChannel(inoutWriter, terrain.Wetness);
    inoutWriter.WriteUInt64(terrain.Masks.GetSize());
    for (const FoliageMaskChannel& channel : terrain.Masks)
    {
        inoutWriter.WriteUInt16(channel.Id);
        WriteChannel(inoutWriter, channel.Values);
    }
}

void WriteRecipe(DigestWriter& inoutWriter, const FoliageCookInput& input) noexcept
{
    inoutWriter.WriteUInt64(input.Species.GetSize());
    for (const FoliageCookSpecies& species : input.Species)
    {
        // RenderRevision is deliberately absent: render-only edits keep the digest and the records.
        inoutWriter.WriteUInt64(species.Key);
        inoutWriter.WriteUInt64(species.PlacementRevision);
        inoutWriter.WriteUInt32(species.VariantCount);
        inoutWriter.WriteUInt16(species.MinimumScale);
        inoutWriter.WriteUInt16(species.MaximumScale);
        inoutWriter.WriteBool(species.IsAlignedToTerrain);
        for (const uint32 radius : species.ExclusionRadii)
        {
            inoutWriter.WriteUInt32(radius);
        }
        inoutWriter.WriteUInt32(species.BoundsRadius);
        inoutWriter.WriteUInt32(species.BoundsHeight);
    }
    inoutWriter.WriteUInt64(input.Layers.GetSize());
    for (const FoliageCookLayer& layer : input.Layers)
    {
        inoutWriter.WriteUInt16(layer.Id);
        inoutWriter.WriteUInt8(layer.Stratum);
        inoutWriter.WriteUInt16(layer.SlotsPerCell);
        inoutWriter.WriteUInt32(layer.CellSize);
        inoutWriter.WriteUInt64(layer.Seed);
        inoutWriter.WriteUInt32(layer.Density);
        inoutWriter.WriteUInt64(layer.Palette.GetSize());
        for (const FoliagePaletteEntry& entry : layer.Palette)
        {
            inoutWriter.WriteUInt16(entry.Species);
            inoutWriter.WriteUInt32(entry.Weight);
        }
        WriteRange(inoutWriter, layer.Height);
        WriteRange(inoutWriter, layer.Slope);
        WriteRange(inoutWriter, layer.Wetness);
        inoutWriter.WriteBool(layer.IsWetnessRequired);
        inoutWriter.WriteUInt64(layer.Masks.GetSize());
        for (const FoliageMaskRule& rule : layer.Masks)
        {
            inoutWriter.WriteUInt16(rule.MaskId);
            inoutWriter.WriteBool(rule.IsRequired);
        }
    }
    inoutWriter.WriteUInt64(input.Suppressions.GetSize());
    for (const FoliageCandidateKey& key : input.Suppressions)
    {
        inoutWriter.WriteUInt16(key.Layer);
        inoutWriter.WriteUInt16(key.Slot);
        inoutWriter.WriteUInt32(static_cast<uint32>(key.CellX));
        inoutWriter.WriteUInt32(static_cast<uint32>(key.CellZ));
    }
}

/// @pre The input is valid.
bool TryDigest(const FoliageCookInput& input, FoliageInputDigest& outDigest) noexcept
{
    DigestWriter writer;
    writer.WriteBytes({reinterpret_cast<const uint8*>(DIGEST_TAG.data()), DIGEST_TAG.size()});
    writer.WriteUInt32(FOLIAGE_COOK_GENERATOR_VERSION);
    writer.WriteUInt32(FOLIAGE_RANDOM_ADDRESS_LAYOUT_VERSION);
    writer.WriteUInt32(format::CURRENT_FORMAT_VERSION);
    writer.WriteUInt32(format::DOMAIN_SCHEMA_1);
    writer.WriteUInt64(input.Domain.Id);
    writer.WriteInt64(input.Domain.OriginX);
    writer.WriteInt64(input.Domain.OriginZ);
    writer.WriteUInt32(input.Domain.PageSize);
    WriteTerrain(writer, input.Terrain);
    WriteRecipe(writer, input);
    return writer.TryFinish(outDigest);
}

FoliageCookStatus
Fail(FoliageCookDiagnostic& outDiagnostic, FoliageCookStatus status, FoliageCookIssue issue, usize index) noexcept
{
    return internal::SetFailure(outDiagnostic, status, issue, index);
}
} // namespace

FoliageCookStatus TryComputeFoliageInputDigest(const FoliageCookInput& input,
                                               FoliageInputDigest& outDigest,
                                               FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const FoliageCookStatus status = ValidateFoliageCookInput(input, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    FoliageInputDigest digest{};
    if (TryDigest(input, digest) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::TerrainGrid, 0);
    }
    outDigest = digest;
    return FoliageCookStatus::Ok;
}

FoliageCookStatus TryWriteFoliageCookedPageSet(const FoliageCookInput& input,
                                               const FoliageCookedPages& pages,
                                               Array<uint8>& outBytes,
                                               FoliageCookDiagnostic& outDiagnostic) noexcept
{
    FoliageInputDigest digest{};
    FoliageCookStatus status = TryComputeFoliageInputDigest(input, digest, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    const BorrowedView<const FoliagePage> cookedPages = pages.GetPages();
    const BorrowedView<const FoliagePlacement> cookedPlacements = pages.GetPlacements();
    Array<usize> order;
    Array<FoliagePage> directory;
    Array<FoliagePlacement> placements;
    Array<FoliageLayer> layers;
    Array<FoliageSpecies> species;
    if (order.TryEnsureCapacity(cookedPages.GetSize()) == false ||
        directory.TryEnsureCapacity(cookedPages.GetSize()) == false ||
        placements.TryEnsureCapacity(cookedPlacements.GetSize()) == false ||
        layers.TryEnsureCapacity(input.Layers.GetSize()) == false ||
        species.TryEnsureCapacity(input.Species.GetSize()) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    for (usize i = 0; i < cookedPages.GetSize(); ++i)
    {
        [[maybe_unused]] const bool isAdded = order.TryAdd(i);
        LUDUS_ASSERT(isAdded == true);
    }
    foundation::Sort(order.GetData(), order.GetSize(), [&cookedPages](usize left, usize right) noexcept {
        const FoliagePage& first = cookedPages[left];
        const FoliagePage& second = cookedPages[right];
        return first.PageZ < second.PageZ || (first.PageZ == second.PageZ && first.PageX < second.PageX);
    });
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        FoliagePage page = cookedPages[order[i]];
        if (i > 0 && page.PageX == directory[i - 1].PageX && page.PageZ == directory[i - 1].PageZ)
        {
            return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::PageRequest, i);
        }
        const BorrowedView<const FoliagePlacement> run{cookedPlacements.GetDataOrNull() + page.FirstPlacement,
                                                       page.PlacementCount};
        page.FirstPlacement = static_cast<uint32>(placements.GetSize());
        [[maybe_unused]] const bool isAdded = directory.TryAdd(page) == true && placements.TryAddRange(run) == true;
        LUDUS_ASSERT(isAdded == true);
    }
    for (const FoliageCookLayer& layer : input.Layers)
    {
        [[maybe_unused]] const bool isAdded = layers.TryAdd(FoliageLayer
        {
            .Id = layer.Id,
            .SlotsPerCell = layer.SlotsPerCell,
            .CellSize = layer.CellSize,
        });
        LUDUS_ASSERT(isAdded == true);
    }
    for (const FoliageCookSpecies& entry : input.Species)
    {
        [[maybe_unused]] const bool isAdded = species.TryAdd(FoliageSpecies
        {
            .Key = entry.Key,
            .PlacementRevision = entry.PlacementRevision,
            .VariantCount = entry.VariantCount,
        });
        LUDUS_ASSERT(isAdded == true);
    }
    foliage_pages::FoliagePageSetDescription description;
    description.Header.GeneratorVersion = FOLIAGE_COOK_GENERATOR_VERSION;
    description.Header.PageSize = input.Domain.PageSize;
    description.Header.DomainId = input.Domain.Id;
    description.Header.OriginX = input.Domain.OriginX;
    description.Header.OriginZ = input.Domain.OriginZ;
    description.Header.TerrainRevision = input.Terrain.Grid.Tiles[0].GetDescription().Revision;
    description.Header.InputDigest = digest;
    description.Layers = {layers.GetData(), layers.GetSize()};
    description.Species = {species.GetData(), species.GetSize()};
    description.Pages = {directory.GetData(), directory.GetSize()};
    description.Placements = {placements.GetData(), placements.GetSize()};
    usize byteCount = 0;
    if (foliage_pages::GetFoliagePageSetBytes(description, {}, byteCount, outDiagnostic.Encode) !=
        foliage_pages::FoliagePageStatus::Ok)
    {
        return Fail(outDiagnostic, FoliageCookStatus::EncodeFailed, FoliageCookIssue::Encode, 0);
    }
    Array<uint8> bytes;
    if (bytes.TryResize(byteCount) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    usize written = 0;
    if (foliage_pages::WriteFoliagePageSet(description,
                                           {},
                                           {bytes.GetData(), bytes.GetSize()},
                                           written,
                                           outDiagnostic.Encode) != foliage_pages::FoliagePageStatus::Ok)
    {
        return Fail(outDiagnostic, FoliageCookStatus::EncodeFailed, FoliageCookIssue::Encode, 0);
    }
    status = FoliageCookStatus::Ok;
    outBytes = foundation::Move(bytes);
    return status;
}
} // namespace ludus::content::foliage_cook
