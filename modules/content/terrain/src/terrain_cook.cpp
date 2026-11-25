#include <ludus/content/terrain/terrain_cook.h>

#include "internal/terrain_tile_heights.h"

#include <ludus/content/terrain_format/terrain_format.h>
#include <ludus/foundation/base/borrowed_view.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <bit>

namespace ludus::content::terrain
{
namespace format = terrain_format;

/// Constructs the private state of TerrainCookedPage for the cooker in this file.
struct TerrainCookedPageAccess final
{
    static TerrainPageRecord& GetRecord(TerrainCookedPage& inoutPage) noexcept
    {
        return inoutPage.mRecord;
    }

    static foundation::Array<uint8>& GetBytes(TerrainCookedPage& inoutPage) noexcept
    {
        return inoutPage.mBytes;
    }
};

/// Constructs the private state of TerrainCookedRevision for the assembler in this file.
struct TerrainCookedRevisionAccess final
{
    static foundation::Array<uint8>& GetManifestBytes(TerrainCookedRevision& inoutRevision) noexcept
    {
        return inoutRevision.mManifestBytes;
    }

    static foundation::Array<uint8>& GetPagePackBytes(TerrainCookedRevision& inoutRevision) noexcept
    {
        return inoutRevision.mPagePackBytes;
    }
};

/// Constructs the private state of TerrainManifest for the reader in this file.
struct TerrainManifestAccess final
{
    static TerrainRevisionLayout& GetLayout(TerrainManifest& inoutManifest) noexcept
    {
        return inoutManifest.mLayout;
    }

    static foundation::Array<TerrainPageRecord>& GetRecords(TerrainManifest& inoutManifest) noexcept
    {
        return inoutManifest.mRecords;
    }
};

/// Constructs and reads the private state of TerrainPage for the reader in this file.
struct TerrainPageAccess final
{
    static TerrainTile& GetTile(TerrainPage& inoutPage) noexcept
    {
        return inoutPage.mTile;
    }

    static foundation::Array<uint8>& GetHaloCodes(TerrainPage& inoutPage) noexcept
    {
        return inoutPage.mHaloCodes;
    }

    static TerrainPageRecord& GetRecord(TerrainPage& inoutPage) noexcept
    {
        return inoutPage.mRecord;
    }

    static foundation::BorrowedView<const uint8> GetHaloCodes(const TerrainPage& page) noexcept
    {
        return {page.mHaloCodes.GetData(), page.mHaloCodes.GetSize()};
    }
};

namespace
{
namespace math = foundation::math;
using foundation::Array;
using foundation::BorrowedView;
using foundation::Fingerprint128;
using foundation::core::uint16;

static_assert(format::MAX_PAGE_AXIS_CELLS <= MAX_TERRAIN_TILE_AXIS_CELLS, "every page is a legal tile");
static_assert(format::MAX_DOMAIN_AXIS_CELLS < MAX_TERRAIN_FIELD_AXIS_SAMPLES,
              "the field kernels accept every admitted domain");
static_assert(static_cast<uint32>(format::HeightEncodingTag::Unsigned16) ==
                      static_cast<uint32>(TerrainHeightEncoding::Unsigned16) &&
                  static_cast<uint32>(format::HeightEncodingTag::Signed32) ==
                      static_cast<uint32>(TerrainHeightEncoding::Signed32),
              "wire tags follow the tile encoding order");

constexpr uint32 HALO_SAMPLES = format::PAGE_HALO_SAMPLES;
constexpr int64 HALO = static_cast<int64>(HALO_SAMPLES);
constexpr usize UNASSIGNED_SLOT = ~usize{0};
constexpr int64 MAX_UNSIGNED16_CODE = 0xffff;
constexpr int64 MAX_SIGNED32_CODE = 0x7fffffff;
constexpr int64 MIN_SIGNED32_CODE = -MAX_SIGNED32_CODE - 1;

format::HeightEncodingTag GetEncodingTag(TerrainHeightEncoding encoding) noexcept
{
    return encoding == TerrainHeightEncoding::Unsigned16 ? format::HeightEncodingTag::Unsigned16
                                                         : format::HeightEncodingTag::Signed32;
}

bool TryGetEncoding(uint32 tag, TerrainHeightEncoding& outEncoding) noexcept
{
    if (tag == static_cast<uint32>(format::HeightEncodingTag::Unsigned16))
    {
        outEncoding = TerrainHeightEncoding::Unsigned16;
        return true;
    }
    if (tag == static_cast<uint32>(format::HeightEncodingTag::Signed32))
    {
        outEncoding = TerrainHeightEncoding::Signed32;
        return true;
    }
    return false;
}

usize GetCodeBytes(TerrainHeightEncoding encoding) noexcept
{
    return format::GetHeightCodeBytes(GetEncodingTag(encoding));
}

usize GetBlobBytes(const TerrainRevisionLayout& layout) noexcept
{
    return format::GetPageBlobBytes(GetEncodingTag(layout.Codec.Encoding), layout.CellsPerPage);
}

usize GetPayloadBytes(const TerrainRevisionLayout& layout) noexcept
{
    return format::GetPagePayloadBytes(GetEncodingTag(layout.Codec.Encoding), layout.CellsPerPage);
}

usize GetPageCount(const TerrainRevisionLayout& layout) noexcept
{
    return static_cast<usize>(layout.PagesX) * layout.PagesZ;
}

bool AreCodecsEqual(const TerrainHeightCodec& first, const TerrainHeightCodec& second) noexcept
{
    return first.Encoding == second.Encoding && first.OriginMetres == second.OriginMetres &&
           first.QuantumMetres == second.QuantumMetres;
}

TerrainTileDescription MakePageDescription(const TerrainRevisionLayout& layout, TerrainTileAddress address) noexcept
{
    TerrainTileDescription description;
    description.Revision = layout.Revision;
    description.Address = address;
    description.CellsPerSide = layout.CellsPerPage;
    description.SpacingMetres = layout.SpacingMetres;
    description.Codec = layout.Codec;
    return description;
}

TerrainCookStatus GetCookStatus(TerrainTileStatus status) noexcept
{
    switch (status)
    {
        case TerrainTileStatus::Success:
            return TerrainCookStatus::Success;
        case TerrainTileStatus::CoordinateOutOfRange:
            return TerrainCookStatus::CoordinateOutOfRange;
        case TerrainTileStatus::HeightOutOfRange:
            return TerrainCookStatus::HeightOutOfRange;
        case TerrainTileStatus::AllocationFailed:
            return TerrainCookStatus::AllocationFailed;
        case TerrainTileStatus::InvalidStorage:
            return TerrainCookStatus::InvalidStorage;
        case TerrainTileStatus::CapacityExceeded:
            return TerrainCookStatus::LimitExceeded;
        case TerrainTileStatus::SeamMismatch:
            return TerrainCookStatus::BorderMismatch;
        case TerrainTileStatus::InvalidArgument:
        case TerrainTileStatus::OutsideDomain:
        case TerrainTileStatus::Miss:
        case TerrainTileStatus::BudgetExceeded:
            break;
    }
    return TerrainCookStatus::InvalidArgument;
}

/// Validate a layout: page size, counts, domain extent, codec, spacing and representable page coordinates.
TerrainCookStatus ValidateLayout(const TerrainRevisionLayout& layout) noexcept
{
    if (layout.CellsPerPage == 0 || layout.CellsPerPage > format::MAX_PAGE_AXIS_CELLS || layout.PagesX == 0 ||
        layout.PagesZ == 0)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    if (static_cast<uint64>(layout.PagesX) * layout.CellsPerPage > format::MAX_DOMAIN_AXIS_CELLS ||
        static_cast<uint64>(layout.PagesZ) * layout.CellsPerPage > format::MAX_DOMAIN_AXIS_CELLS ||
        GetPageCount(layout) > format::MAX_REVISION_PAGES)
    {
        return TerrainCookStatus::LimitExceeded;
    }
    const TerrainTileStatus status =
        internal::ValidateTerrainTileDescription(MakePageDescription(layout, layout.FirstPage));
    if (status != TerrainTileStatus::Success)
    {
        return GetCookStatus(status);
    }
    TerrainTileAddress lastPage;
    TerrainSampleAddress lastOrigin;
    if (foundation::TryAdd(layout.FirstPage.X, static_cast<int64>(layout.PagesX) - 1, lastPage.X) == false ||
        foundation::TryAdd(layout.FirstPage.Z, static_cast<int64>(layout.PagesZ) - 1, lastPage.Z) == false ||
        TryGetTerrainTileOrigin(lastPage, layout.CellsPerPage, lastOrigin) != TerrainTileStatus::Success)
    {
        return TerrainCookStatus::CoordinateOutOfRange;
    }
    return TerrainCookStatus::Success;
}

/// Global sample of field sample (0, 0); valid for a validated layout.
TerrainSampleAddress GetDomainOrigin(const TerrainRevisionLayout& layout) noexcept
{
    TerrainSampleAddress origin;
    LUDUS_REQUIRE(TryGetTerrainTileOrigin(layout.FirstPage, layout.CellsPerPage, origin) == TerrainTileStatus::Success);
    return origin;
}

/// Domain field samples on each axis; valid for a validated layout.
uint32 GetDomainColumns(const TerrainRevisionLayout& layout) noexcept
{
    return (layout.PagesX * layout.CellsPerPage) + 1;
}

uint32 GetDomainRows(const TerrainRevisionLayout& layout) noexcept
{
    return (layout.PagesZ * layout.CellsPerPage) + 1;
}

TerrainCookStatus ValidateField(const TerrainRevisionLayout& layout,
                                const TerrainFieldGrid& grid,
                                BorrowedView<const float32> elevationsMetres) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false || grid.Columns != GetDomainColumns(layout) ||
        grid.Rows != GetDomainRows(layout) || grid.SpacingXMetres != layout.SpacingMetres ||
        grid.SpacingZMetres != layout.SpacingMetres)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    if (elevationsMetres.IsValid() == false || elevationsMetres.GetSize() != GetTerrainSampleCount(grid))
    {
        return TerrainCookStatus::InvalidStorage;
    }
    return TerrainCookStatus::Success;
}

/// Canonical index of a page address, or false outside the domain.
bool TryGetPageIndex(const TerrainRevisionLayout& layout, TerrainTileAddress address, usize& outIndex) noexcept
{
    int64 column = 0;
    int64 row = 0;
    if (foundation::TrySubtract(address.X, layout.FirstPage.X, column) == false ||
        foundation::TrySubtract(address.Z, layout.FirstPage.Z, row) == false || column < 0 || row < 0 ||
        column >= static_cast<int64>(layout.PagesX) || row >= static_cast<int64>(layout.PagesZ))
    {
        return false;
    }
    outIndex = (static_cast<usize>(row) * layout.PagesX) + static_cast<usize>(column);
    return true;
}

TerrainTileAddress GetPageAddress(const TerrainRevisionLayout& layout, usize index) noexcept
{
    return {layout.FirstPage.X + static_cast<int64>(index % layout.PagesX),
            layout.FirstPage.Z + static_cast<int64>(index / layout.PagesX)};
}

/// Page-local column and row of an extended-window sample, each in [-HALO, CellsPerPage + HALO].
struct PageSample final
{
    int64 Column = 0;
    int64 Row = 0;
};

/// Index of a page-local extended-window sample in the payload: the core in row-major order, then the halo.
usize GetPayloadSampleIndex(uint32 cellsPerPage, PageSample sample) noexcept
{
    const int64 column = sample.Column;
    const int64 row = sample.Row;
    const int64 coreSamples = static_cast<int64>(cellsPerPage) + 1;
    if (column >= 0 && column < coreSamples && row >= 0 && row < coreSamples)
    {
        return static_cast<usize>((row * coreSamples) + column);
    }
    const int64 extendedSamples = coreSamples + (2 * HALO);
    const int64 haloBase = coreSamples * coreSamples;
    int64 haloIndex = 0;
    if (row < 0)
    {
        haloIndex = ((row + HALO) * extendedSamples) + (column + HALO);
    }
    else if (row < coreSamples)
    {
        const int64 rowStart = (HALO * extendedSamples) + (row * 2 * HALO);
        haloIndex = rowStart + (column < 0 ? column + HALO : HALO + (column - coreSamples));
    }
    else
    {
        haloIndex = (HALO * extendedSamples) + (coreSamples * 2 * HALO) + ((row - coreSamples) * extendedSamples) +
                    (column + HALO);
    }
    return static_cast<usize>(haloBase + haloIndex);
}

int64 ReadCode(BorrowedView<const uint8> payload, TerrainHeightEncoding encoding, usize sampleIndex) noexcept
{
    const usize width = GetCodeBytes(encoding);
    const foundation::BorrowedView<const uint8> bytes{payload.GetDataOrNull() + (sampleIndex * width), width};
    if (encoding == TerrainHeightEncoding::Unsigned16)
    {
        uint16 code = 0;
        LUDUS_REQUIRE(foundation::TryReadLittleEndian(bytes, code));
        return code;
    }
    uint32 code = 0;
    LUDUS_REQUIRE(foundation::TryReadLittleEndian(bytes, code));
    // C++23 defines this conversion modulo the destination width, including negative two's-complement codes.
    return static_cast<int32>(code);
}

void WriteCode(int64 code, TerrainHeightEncoding encoding, usize sampleIndex, BorrowedView<uint8> outPayload) noexcept
{
    const usize width = GetCodeBytes(encoding);
    const foundation::BorrowedView<uint8> bytes{outPayload.GetDataOrNull() + (sampleIndex * width), width};
    if (encoding == TerrainHeightEncoding::Unsigned16)
    {
        LUDUS_REQUIRE(foundation::TryWriteLittleEndian(static_cast<uint16>(code), bytes));
        return;
    }
    LUDUS_REQUIRE(foundation::TryWriteLittleEndian(static_cast<uint32>(code), bytes));
}

bool IsCodeInRange(TerrainHeightEncoding encoding, int64 code) noexcept
{
    if (encoding == TerrainHeightEncoding::Unsigned16)
    {
        return code >= 0 && code <= MAX_UNSIGNED16_CODE;
    }
    return code >= MIN_SIGNED32_CODE && code <= MAX_SIGNED32_CODE;
}

/// Sequential little-endian writer into a presized byte array.
class ByteWriter final
{
public:
    explicit ByteWriter(BorrowedView<uint8> outBytes) noexcept : mBytes(outBytes) {}

    void WriteMagic(std::string_view magic) noexcept
    {
        LUDUS_REQUIRE(magic.size() <= mBytes.GetSize() - mPosition);
        for (usize i = 0; i < magic.size(); ++i)
        {
            mBytes[mPosition + i] = static_cast<uint8>(magic[i]);
        }
        mPosition += magic.size();
    }

    template <typename ValueType>
    void WriteUnsigned(ValueType value) noexcept
    {
        LUDUS_REQUIRE(sizeof(ValueType) <= mBytes.GetSize() - mPosition);
        LUDUS_REQUIRE(foundation::TryWriteLittleEndian(value, {mBytes.GetDataOrNull() + mPosition, sizeof(ValueType)}));
        mPosition += sizeof(ValueType);
    }

    void WriteUint32(uint32 value) noexcept
    {
        WriteUnsigned(value);
    }

    void WriteUint64(uint64 value) noexcept
    {
        WriteUnsigned(value);
    }

    void WriteInt32(int32 value) noexcept
    {
        WriteUnsigned(static_cast<uint32>(value));
    }

    void WriteInt64(int64 value) noexcept
    {
        WriteUnsigned(static_cast<uint64>(value));
    }

    void WriteFloat32(float32 value) noexcept
    {
        WriteUnsigned(std::bit_cast<uint32>(value));
    }

    void WriteDigest(Fingerprint128 digest) noexcept
    {
        WriteUint64(digest.Low);
        WriteUint64(digest.High);
    }

    [[nodiscard]] usize GetPosition() const noexcept
    {
        return mPosition;
    }

private:
    BorrowedView<uint8> mBytes;
    usize mPosition = 0;
};

/// Sequential little-endian reader over a bounded byte view; reads past the end fail and stick.
class ByteReader final
{
public:
    explicit ByteReader(BorrowedView<const uint8> bytes) noexcept : mBytes(bytes) {}

    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }

    [[nodiscard]] bool TryReadMagic(std::string_view expected) noexcept
    {
        if (canRead(expected.size()) == false)
        {
            return false;
        }
        bool isEqual = true;
        for (usize i = 0; i < expected.size(); ++i)
        {
            isEqual = isEqual && mBytes[mPosition + i] == static_cast<uint8>(expected[i]);
        }
        mPosition += expected.size();
        return isEqual;
    }

    template <typename ValueType>
    [[nodiscard]] ValueType ReadUnsigned() noexcept
    {
        ValueType value{};
        if (canRead(sizeof(ValueType)) == false)
        {
            return value;
        }
        LUDUS_REQUIRE(foundation::TryReadLittleEndian({mBytes.GetDataOrNull() + mPosition, sizeof(ValueType)}, value));
        mPosition += sizeof(ValueType);
        return value;
    }

    [[nodiscard]] uint32 ReadUint32() noexcept
    {
        return ReadUnsigned<uint32>();
    }

    [[nodiscard]] uint64 ReadUint64() noexcept
    {
        return ReadUnsigned<uint64>();
    }

    [[nodiscard]] int32 ReadInt32() noexcept
    {
        return static_cast<int32>(ReadUnsigned<uint32>());
    }

    [[nodiscard]] int64 ReadInt64() noexcept
    {
        return static_cast<int64>(ReadUnsigned<uint64>());
    }

    [[nodiscard]] float32 ReadFloat32() noexcept
    {
        return std::bit_cast<float32>(ReadUnsigned<uint32>());
    }

    [[nodiscard]] Fingerprint128 ReadDigest() noexcept
    {
        Fingerprint128 digest;
        digest.Low = ReadUint64();
        digest.High = ReadUint64();
        return digest;
    }

private:
    [[nodiscard]] bool canRead(usize count) noexcept
    {
        if (mHasFailed == true || count > mBytes.GetSize() - mPosition)
        {
            mHasFailed = true;
            return false;
        }
        return true;
    }

    BorrowedView<const uint8> mBytes;
    usize mPosition = 0;
    bool mHasFailed = false;
};

Fingerprint128 ComputeDigest(BorrowedView<const uint8> bytes) noexcept
{
    return foundation::StableFingerprint128(foundation::MakeSpan(bytes));
}

/// Fields of a page header that identify the page.
struct PageIdentity final
{
    TerrainTileAddress Address{};
    uint64 ContentRevision = 0;
};

void WritePageHeader(const TerrainRevisionLayout& layout, PageIdentity identity, ByteWriter& inoutWriter) noexcept
{
    inoutWriter.WriteMagic(format::PAGE_MAGIC);
    inoutWriter.WriteUint32(format::FORMAT_VERSION);
    inoutWriter.WriteUint32(static_cast<uint32>(format::ChannelTag::Height));
    inoutWriter.WriteUint32(0);
    inoutWriter.WriteUint32(static_cast<uint32>(GetEncodingTag(layout.Codec.Encoding)));
    inoutWriter.WriteUint32(layout.CellsPerPage);
    inoutWriter.WriteUint32(HALO_SAMPLES);
    inoutWriter.WriteInt64(identity.Address.X);
    inoutWriter.WriteInt64(identity.Address.Z);
    inoutWriter.WriteUint64(identity.ContentRevision);
    inoutWriter.WriteFloat32(layout.Codec.OriginMetres);
    inoutWriter.WriteFloat32(layout.Codec.QuantumMetres);
    inoutWriter.WriteFloat32(layout.SpacingMetres);
    inoutWriter.WriteUint32(static_cast<uint32>(GetPayloadBytes(layout)));
    LUDUS_REQUIRE(inoutWriter.GetPosition() == format::PAGE_HEADER_BYTES);
}

/// Validate a page blob's header against a layout and return its identity. The payload is not inspected.
TerrainCookStatus
ReadPageHeader(const TerrainRevisionLayout& layout, BorrowedView<const uint8> blob, PageIdentity& outIdentity) noexcept
{
    if (blob.IsValid() == false)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    ByteReader reader(blob);
    if (reader.TryReadMagic(format::PAGE_MAGIC) == false)
    {
        return TerrainCookStatus::MalformedData;
    }
    if (reader.ReadUint32() != format::FORMAT_VERSION)
    {
        return reader.HasFailed() == true ? TerrainCookStatus::MalformedData : TerrainCookStatus::UnsupportedVersion;
    }
    const uint32 channel = reader.ReadUint32();
    const uint32 level = reader.ReadUint32();
    const uint32 encoding = reader.ReadUint32();
    const uint32 cellsPerSide = reader.ReadUint32();
    const uint32 haloSamples = reader.ReadUint32();
    PageIdentity identity;
    identity.Address.X = reader.ReadInt64();
    identity.Address.Z = reader.ReadInt64();
    identity.ContentRevision = reader.ReadUint64();
    const float32 originMetres = reader.ReadFloat32();
    const float32 quantumMetres = reader.ReadFloat32();
    const float32 spacingMetres = reader.ReadFloat32();
    const uint32 payloadBytes = reader.ReadUint32();
    TerrainHeightEncoding decodedEncoding = TerrainHeightEncoding::Unsigned16;
    if (reader.HasFailed() == true || channel != static_cast<uint32>(format::ChannelTag::Height) || level != 0 ||
        TryGetEncoding(encoding, decodedEncoding) == false)
    {
        return TerrainCookStatus::MalformedData;
    }
    if (decodedEncoding != layout.Codec.Encoding || cellsPerSide != layout.CellsPerPage ||
        haloSamples != HALO_SAMPLES || originMetres != layout.Codec.OriginMetres ||
        quantumMetres != layout.Codec.QuantumMetres || spacingMetres != layout.SpacingMetres)
    {
        return TerrainCookStatus::PageMismatch;
    }
    if (payloadBytes != GetPayloadBytes(layout) || blob.GetSize() != GetBlobBytes(layout))
    {
        return TerrainCookStatus::MalformedData;
    }
    outIdentity = identity;
    return TerrainCookStatus::Success;
}

BorrowedView<const uint8> GetPayload(BorrowedView<const uint8> blob) noexcept
{
    return {blob.GetDataOrNull() + format::PAGE_HEADER_BYTES, blob.GetSize() - format::PAGE_HEADER_BYTES};
}

/// Lowest and highest core code of a payload.
struct CodeBounds final
{
    int64 Minimum = 0;
    int64 Maximum = 0;
};

CodeBounds ScanCoreCodes(const TerrainRevisionLayout& layout, BorrowedView<const uint8> payload) noexcept
{
    const usize coreSamples = static_cast<usize>(layout.CellsPerPage) + 1;
    CodeBounds bounds;
    bounds.Minimum = ReadCode(payload, layout.Codec.Encoding, 0);
    bounds.Maximum = bounds.Minimum;
    for (usize i = 1; i < coreSamples * coreSamples; ++i)
    {
        const int64 code = ReadCode(payload, layout.Codec.Encoding, i);
        bounds.Minimum = code < bounds.Minimum ? code : bounds.Minimum;
        bounds.Maximum = code > bounds.Maximum ? code : bounds.Maximum;
    }
    return bounds;
}

/// Validate a blob against its manifest layout and record: header identity, size, digest and core code bounds.
TerrainCookStatus ValidatePageBlob(const TerrainRevisionLayout& layout,
                                   const TerrainPageRecord& record,
                                   BorrowedView<const uint8> blob) noexcept
{
    PageIdentity identity;
    const TerrainCookStatus status = ReadPageHeader(layout, blob, identity);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    if (blob.GetSize() != record.ByteSize)
    {
        return TerrainCookStatus::MalformedData;
    }
    if (identity.Address.X != record.Address.X || identity.Address.Z != record.Address.Z ||
        identity.ContentRevision != record.ContentRevision)
    {
        return TerrainCookStatus::PageMismatch;
    }
    const BorrowedView<const uint8> payload = GetPayload(blob);
    if (ComputeDigest(payload) != record.PayloadDigest)
    {
        return TerrainCookStatus::DigestMismatch;
    }
    const CodeBounds bounds = ScanCoreCodes(layout, payload);
    if (bounds.Minimum != record.MinimumCode || bounds.Maximum != record.MaximumCode)
    {
        return TerrainCookStatus::MalformedData;
    }
    return TerrainCookStatus::Success;
}

int64 ClampSample(int64 value, int64 maximum) noexcept
{
    return value < 0 ? 0 : (value > maximum ? maximum : value);
}

/// Compare a page's border and halo samples with the pages that own them.
TerrainCookStatus CheckPageBorders(const TerrainRevisionLayout& layout,
                                   BorrowedView<const TerrainCookedPage> pages,
                                   BorrowedView<const usize> slots,
                                   usize slot) noexcept
{
    const int64 cells = layout.CellsPerPage;
    const int64 lastColumn = static_cast<int64>(GetDomainColumns(layout)) - 1;
    const int64 lastRow = static_cast<int64>(GetDomainRows(layout)) - 1;
    const int64 pageColumn = static_cast<int64>(slot % layout.PagesX);
    const int64 pageRow = static_cast<int64>(slot / layout.PagesX);
    const BorrowedView<const uint8> payload = GetPayload(pages[slots[slot]].GetBytes());
    for (int64 row = -HALO; row <= cells + HALO; ++row)
    {
        for (int64 column = -HALO; column <= cells + HALO; ++column)
        {
            const bool isInterior = column > 0 && column < cells && row > 0 && row < cells;
            if (isInterior == true)
            {
                continue;
            }
            const int64 fieldColumn = ClampSample((pageColumn * cells) + column, lastColumn);
            const int64 fieldRow = ClampSample((pageRow * cells) + row, lastRow);
            const int64 ownerColumn = fieldColumn / cells < layout.PagesX ? fieldColumn / cells : layout.PagesX - 1;
            const int64 ownerRow = fieldRow / cells < layout.PagesZ ? fieldRow / cells : layout.PagesZ - 1;
            const int64 ownerLocalColumn = fieldColumn - (ownerColumn * cells);
            const int64 ownerLocalRow = fieldRow - (ownerRow * cells);
            const usize ownerSlot = static_cast<usize>((ownerRow * layout.PagesX) + ownerColumn);
            if (ownerSlot == slot && ownerLocalColumn == column && ownerLocalRow == row)
            {
                continue;
            }
            const BorrowedView<const uint8> ownerPayload = GetPayload(pages[slots[ownerSlot]].GetBytes());
            const int64 code =
                ReadCode(payload, layout.Codec.Encoding, GetPayloadSampleIndex(layout.CellsPerPage, {column, row}));
            const int64 ownerCode =
                ReadCode(ownerPayload,
                         layout.Codec.Encoding,
                         GetPayloadSampleIndex(layout.CellsPerPage, {ownerLocalColumn, ownerLocalRow}));
            if (code != ownerCode)
            {
                return TerrainCookStatus::BorderMismatch;
            }
        }
    }
    return TerrainCookStatus::Success;
}

void WriteManifest(const TerrainRevisionLayout& layout,
                   BorrowedView<const TerrainPageRecord> records,
                   uint64 pagePackBytes,
                   BorrowedView<uint8> outBytes) noexcept
{
    ByteWriter writer(outBytes);
    writer.WriteMagic(format::MANIFEST_MAGIC);
    writer.WriteUint32(format::FORMAT_VERSION);
    writer.WriteUint32(format::COOKER_VERSION);
    writer.WriteUint32(layout.RecipeVersion);
    writer.WriteUint32(static_cast<uint32>(GetEncodingTag(layout.Codec.Encoding)));
    writer.WriteUint32(layout.CellsPerPage);
    writer.WriteUint32(HALO_SAMPLES);
    writer.WriteUint32(static_cast<uint32>(format::BoundaryExtensionTag::ClampToEdge));
    writer.WriteUint32(layout.PagesX);
    writer.WriteUint32(layout.PagesZ);
    writer.WriteUint32(0);
    writer.WriteUint64(layout.Revision);
    writer.WriteInt64(layout.FirstPage.X);
    writer.WriteInt64(layout.FirstPage.Z);
    writer.WriteFloat32(layout.Codec.OriginMetres);
    writer.WriteFloat32(layout.Codec.QuantumMetres);
    writer.WriteFloat32(layout.SpacingMetres);
    writer.WriteUint32(static_cast<uint32>(records.GetSize()));
    writer.WriteDigest(layout.SourceDigest);
    writer.WriteUint64(pagePackBytes);
    LUDUS_REQUIRE(writer.GetPosition() == format::MANIFEST_HEADER_BYTES);
    for (const TerrainPageRecord& record : records)
    {
        writer.WriteInt64(record.Address.X);
        writer.WriteInt64(record.Address.Z);
        writer.WriteUint64(record.ContentRevision);
        writer.WriteUint64(record.ByteOffset);
        writer.WriteUint32(record.ByteSize);
        writer.WriteUint32(static_cast<uint32>(format::ChannelTag::Height));
        writer.WriteUint32(0);
        writer.WriteInt32(record.MinimumCode);
        writer.WriteInt32(record.MaximumCode);
        writer.WriteFloat32(record.MaximumErrorMetres);
        writer.WriteDigest(record.PayloadDigest);
    }
    const usize digestedBytes = writer.GetPosition();
    writer.WriteDigest(ComputeDigest({outBytes.GetDataOrNull(), digestedBytes}));
    LUDUS_REQUIRE(writer.GetPosition() == outBytes.GetSize());
}

TerrainCookStatus CookPage(const TerrainRevisionLayout& layout,
                           const TerrainFieldGrid& grid,
                           BorrowedView<const float32> elevationsMetres,
                           usize pageIndex,
                           TerrainCookedPage& outPage) noexcept
{
    TerrainCookedPage candidate;
    Array<uint8>& bytes = TerrainCookedPageAccess::GetBytes(candidate);
    if (bytes.TryResize(GetBlobBytes(layout)) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    TerrainPageRecord& record = TerrainCookedPageAccess::GetRecord(candidate);
    record.Address = GetPageAddress(layout, pageIndex);
    record.ContentRevision = layout.Revision;
    record.ByteSize = static_cast<uint32>(bytes.GetSize());
    ByteWriter writer(bytes.AsView());
    WritePageHeader(layout, {record.Address, record.ContentRevision}, writer);

    const BorrowedView<uint8> payload{bytes.GetData() + format::PAGE_HEADER_BYTES,
                                      bytes.GetSize() - format::PAGE_HEADER_BYTES};
    const int64 cells = layout.CellsPerPage;
    const int64 lastColumn = static_cast<int64>(grid.Columns) - 1;
    const int64 lastRow = static_cast<int64>(grid.Rows) - 1;
    const int64 pageColumn = static_cast<int64>(pageIndex % layout.PagesX);
    const int64 pageRow = static_cast<int64>(pageIndex / layout.PagesX);
    int64 minimum = 0;
    int64 maximum = 0;
    float64 maximumError = 0;
    bool hasCoreCode = false;
    for (int64 row = -HALO; row <= cells + HALO; ++row)
    {
        for (int64 column = -HALO; column <= cells + HALO; ++column)
        {
            const int64 fieldColumn = ClampSample((pageColumn * cells) + column, lastColumn);
            const int64 fieldRow = ClampSample((pageRow * cells) + row, lastRow);
            const float64 elevation = static_cast<float64>(
                elevationsMetres[(static_cast<usize>(fieldRow) * grid.Columns) + static_cast<usize>(fieldColumn)]);
            int64 code = 0;
            if (internal::TryEncodeTerrainHeight(layout.Codec, elevation, code) == false)
            {
                return TerrainCookStatus::HeightOutOfRange;
            }
            WriteCode(code, layout.Codec.Encoding, GetPayloadSampleIndex(layout.CellsPerPage, {column, row}), payload);
            const bool isCore = column >= 0 && column <= cells && row >= 0 && row <= cells;
            if (isCore == true)
            {
                minimum = hasCoreCode == false || code < minimum ? code : minimum;
                maximum = hasCoreCode == false || code > maximum ? code : maximum;
                hasCoreCode = true;
                const float64 error = math::Abs(internal::DecodeTerrainHeight(layout.Codec, code) - elevation);
                maximumError = error > maximumError ? error : maximumError;
            }
        }
    }
    record.MinimumCode = static_cast<int32>(minimum);
    record.MaximumCode = static_cast<int32>(maximum);
    record.MaximumErrorMetres = static_cast<float32>(maximumError);
    record.PayloadDigest = ComputeDigest(payload);
    outPage = foundation::Move(candidate);
    return TerrainCookStatus::Success;
}

TerrainCookStatus ExtractPage(const TerrainManifest& manifest,
                              BorrowedView<const uint8> pagePackBytes,
                              usize pageIndex,
                              TerrainCookedPage& outPage) noexcept
{
    const TerrainPageRecord& record = manifest.GetPageRecords()[pageIndex];
    const BorrowedView<const uint8> blob{pagePackBytes.GetDataOrNull() + record.ByteOffset, record.ByteSize};
    const TerrainCookStatus status = ValidatePageBlob(manifest.GetLayout(), record, blob);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    TerrainCookedPage candidate;
    if (TerrainCookedPageAccess::GetBytes(candidate).TryAddRange(blob) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    TerrainCookedPageAccess::GetRecord(candidate) = record;
    outPage = foundation::Move(candidate);
    return TerrainCookStatus::Success;
}

TerrainCookStatus ValidatePagePack(const TerrainManifest& manifest, BorrowedView<const uint8> pagePackBytes) noexcept
{
    if (manifest.IsEmpty() == true)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    if (pagePackBytes.IsValid() == false)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    const TerrainPageRecord& last = manifest.GetPageRecords()[manifest.GetPageRecords().GetSize() - 1];
    if (pagePackBytes.GetSize() != last.ByteOffset + last.ByteSize)
    {
        return TerrainCookStatus::MalformedData;
    }
    return TerrainCookStatus::Success;
}

bool AreLayoutsCompatible(const TerrainRevisionLayout& base, const TerrainRevisionLayout& layout) noexcept
{
    return base.FirstPage.X == layout.FirstPage.X && base.FirstPage.Z == layout.FirstPage.Z &&
           base.PagesX == layout.PagesX && base.PagesZ == layout.PagesZ && base.CellsPerPage == layout.CellsPerPage &&
           base.SpacingMetres == layout.SpacingMetres && AreCodecsEqual(base.Codec, layout.Codec) == true;
}

bool DoesRangeIntersect(int64 firstMinimum, int64 firstMaximum, int64 secondMinimum, int64 secondMaximum) noexcept
{
    return firstMinimum <= secondMaximum && secondMinimum <= firstMaximum;
}
} // namespace

TerrainCookStatus TryCookTerrainPage(const TerrainRevisionLayout& layout,
                                     const TerrainFieldGrid& grid,
                                     BorrowedView<const float32> elevationsMetres,
                                     TerrainTileAddress page,
                                     TerrainCookedPage& outPage) noexcept
{
    TerrainCookStatus status = ValidateLayout(layout);
    if (status == TerrainCookStatus::Success)
    {
        status = ValidateField(layout, grid, elevationsMetres);
    }
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    usize pageIndex = 0;
    if (TryGetPageIndex(layout, page, pageIndex) == false)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    return CookPage(layout, grid, elevationsMetres, pageIndex, outPage);
}

TerrainCookStatus TryAssembleTerrainRevision(const TerrainRevisionLayout& layout,
                                             BorrowedView<const TerrainCookedPage> pages,
                                             TerrainCookedRevision& outRevision) noexcept
{
    TerrainCookStatus status = ValidateLayout(layout);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    if (pages.IsValid() == false)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    const usize pageCount = GetPageCount(layout);
    if (pages.GetSize() != pageCount)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    Array<usize> slots;
    if (slots.TryResize(pageCount, UNASSIGNED_SLOT) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    for (usize i = 0; i < pageCount; ++i)
    {
        const TerrainCookedPage& page = pages[i];
        if (page.IsEmpty() == true)
        {
            return TerrainCookStatus::InvalidStorage;
        }
        const TerrainPageRecord& record = page.GetRecord();
        usize slot = 0;
        if (TryGetPageIndex(layout, record.Address, slot) == false || record.ContentRevision > layout.Revision)
        {
            return TerrainCookStatus::PageMismatch;
        }
        if (slots[slot] != UNASSIGNED_SLOT)
        {
            return TerrainCookStatus::InvalidArgument;
        }
        status = ValidatePageBlob(layout, record, page.GetBytes());
        if (status != TerrainCookStatus::Success)
        {
            return status;
        }
        if (math::IsFinite(record.MaximumErrorMetres) == false || record.MaximumErrorMetres < 0)
        {
            return TerrainCookStatus::MalformedData;
        }
        slots[slot] = i;
    }
    for (usize slot = 0; slot < pageCount; ++slot)
    {
        status = CheckPageBorders(layout, pages, slots.AsView(), slot);
        if (status != TerrainCookStatus::Success)
        {
            return status;
        }
    }

    Array<TerrainPageRecord> records;
    if (records.TryResize(pageCount) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    uint64 pagePackBytes = 0;
    for (usize slot = 0; slot < pageCount; ++slot)
    {
        records[slot] = pages[slots[slot]].GetRecord();
        records[slot].ByteOffset = pagePackBytes;
        pagePackBytes += records[slot].ByteSize;
    }
    TerrainCookedRevision candidate;
    Array<uint8>& manifestBytes = TerrainCookedRevisionAccess::GetManifestBytes(candidate);
    Array<uint8>& pagePack = TerrainCookedRevisionAccess::GetPagePackBytes(candidate);
    if (manifestBytes.TryResize(format::GetManifestBytes(static_cast<uint32>(pageCount))) == false ||
        pagePackBytes > pagePack.GetMaxSize() || pagePack.TryEnsureCapacity(static_cast<usize>(pagePackBytes)) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    for (usize slot = 0; slot < pageCount; ++slot)
    {
        LUDUS_REQUIRE(pagePack.TryAddRange(pages[slots[slot]].GetBytes()));
    }
    WriteManifest(layout, records.AsView(), pagePackBytes, manifestBytes.AsView());
    outRevision = foundation::Move(candidate);
    return TerrainCookStatus::Success;
}

TerrainCookStatus TryCookTerrainRevision(const TerrainRevisionLayout& layout,
                                         const TerrainFieldGrid& grid,
                                         BorrowedView<const float32> elevationsMetres,
                                         TerrainCookedRevision& outRevision) noexcept
{
    TerrainCookStatus status = ValidateLayout(layout);
    if (status == TerrainCookStatus::Success)
    {
        status = ValidateField(layout, grid, elevationsMetres);
    }
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    Array<TerrainCookedPage> pages;
    if (pages.TryResize(GetPageCount(layout)) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    for (usize i = 0; i < pages.GetSize(); ++i)
    {
        status = CookPage(layout, grid, elevationsMetres, i, pages[i]);
        if (status != TerrainCookStatus::Success)
        {
            return status;
        }
    }
    return TryAssembleTerrainRevision(layout, pages.AsView(), outRevision);
}

TerrainCookStatus TryReadTerrainManifest(BorrowedView<const uint8> manifestBytes, TerrainManifest& outManifest) noexcept
{
    if (manifestBytes.IsValid() == false)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    if (manifestBytes.GetSize() > format::MAX_MANIFEST_BYTES)
    {
        return TerrainCookStatus::LimitExceeded;
    }
    ByteReader reader(manifestBytes);
    if (reader.TryReadMagic(format::MANIFEST_MAGIC) == false)
    {
        return TerrainCookStatus::MalformedData;
    }
    if (reader.ReadUint32() != format::FORMAT_VERSION)
    {
        return reader.HasFailed() == true ? TerrainCookStatus::MalformedData : TerrainCookStatus::UnsupportedVersion;
    }
    static_cast<void>(reader.ReadUint32()); // Cooker version: a cache key, never an admission condition.
    TerrainRevisionLayout layout;
    layout.RecipeVersion = reader.ReadUint32();
    const uint32 encoding = reader.ReadUint32();
    layout.CellsPerPage = reader.ReadUint32();
    const uint32 haloSamples = reader.ReadUint32();
    const uint32 boundaryExtension = reader.ReadUint32();
    layout.PagesX = reader.ReadUint32();
    layout.PagesZ = reader.ReadUint32();
    const uint32 flags = reader.ReadUint32();
    layout.Revision = reader.ReadUint64();
    layout.FirstPage.X = reader.ReadInt64();
    layout.FirstPage.Z = reader.ReadInt64();
    layout.Codec.OriginMetres = reader.ReadFloat32();
    layout.Codec.QuantumMetres = reader.ReadFloat32();
    layout.SpacingMetres = reader.ReadFloat32();
    const uint32 pageCount = reader.ReadUint32();
    layout.SourceDigest = reader.ReadDigest();
    const uint64 pagePackBytes = reader.ReadUint64();
    if (reader.HasFailed() == true || pageCount > format::MAX_REVISION_PAGES ||
        manifestBytes.GetSize() != format::GetManifestBytes(pageCount))
    {
        return pageCount > format::MAX_REVISION_PAGES ? TerrainCookStatus::LimitExceeded
                                                      : TerrainCookStatus::MalformedData;
    }
    const usize digestedBytes = manifestBytes.GetSize() - format::DIGEST_BYTES;
    ByteReader digestReader({manifestBytes.GetDataOrNull() + digestedBytes, format::DIGEST_BYTES});
    if (ComputeDigest({manifestBytes.GetDataOrNull(), digestedBytes}) != digestReader.ReadDigest())
    {
        return TerrainCookStatus::DigestMismatch;
    }
    if (TryGetEncoding(encoding, layout.Codec.Encoding) == false || haloSamples != HALO_SAMPLES ||
        boundaryExtension != static_cast<uint32>(format::BoundaryExtensionTag::ClampToEdge) || flags != 0)
    {
        return TerrainCookStatus::MalformedData;
    }
    const TerrainCookStatus layoutStatus = ValidateLayout(layout);
    if (layoutStatus != TerrainCookStatus::Success)
    {
        return layoutStatus == TerrainCookStatus::LimitExceeded ? layoutStatus : TerrainCookStatus::MalformedData;
    }
    if (pageCount != GetPageCount(layout))
    {
        return TerrainCookStatus::MalformedData;
    }

    TerrainManifest candidate;
    Array<TerrainPageRecord>& records = TerrainManifestAccess::GetRecords(candidate);
    if (records.TryResize(pageCount) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    const uint64 blobBytes = GetBlobBytes(layout);
    uint64 offset = 0;
    for (usize i = 0; i < pageCount; ++i)
    {
        TerrainPageRecord& record = records[i];
        record.Address.X = reader.ReadInt64();
        record.Address.Z = reader.ReadInt64();
        record.ContentRevision = reader.ReadUint64();
        record.ByteOffset = reader.ReadUint64();
        record.ByteSize = reader.ReadUint32();
        const uint32 channel = reader.ReadUint32();
        const uint32 level = reader.ReadUint32();
        record.MinimumCode = reader.ReadInt32();
        record.MaximumCode = reader.ReadInt32();
        record.MaximumErrorMetres = reader.ReadFloat32();
        record.PayloadDigest = reader.ReadDigest();
        const TerrainTileAddress expected = GetPageAddress(layout, i);
        if (reader.HasFailed() == true || record.Address.X != expected.X || record.Address.Z != expected.Z ||
            record.ContentRevision > layout.Revision || record.ByteOffset != offset || record.ByteSize != blobBytes ||
            channel != static_cast<uint32>(format::ChannelTag::Height) || level != 0 ||
            record.MinimumCode > record.MaximumCode ||
            IsCodeInRange(layout.Codec.Encoding, record.MinimumCode) == false ||
            IsCodeInRange(layout.Codec.Encoding, record.MaximumCode) == false ||
            math::IsFinite(record.MaximumErrorMetres) == false || record.MaximumErrorMetres < 0)
        {
            return TerrainCookStatus::MalformedData;
        }
        offset += blobBytes;
    }
    if (offset != pagePackBytes)
    {
        return TerrainCookStatus::MalformedData;
    }
    TerrainManifestAccess::GetLayout(candidate) = layout;
    outManifest = foundation::Move(candidate);
    return TerrainCookStatus::Success;
}

TerrainCookStatus TryReadTerrainPage(const TerrainManifest& manifest,
                                     usize pageIndex,
                                     BorrowedView<const uint8> pageBytes,
                                     TerrainPage& outPage) noexcept
{
    if (manifest.IsEmpty() == true || pageIndex >= manifest.GetPageRecords().GetSize())
    {
        return TerrainCookStatus::InvalidArgument;
    }
    const TerrainRevisionLayout& layout = manifest.GetLayout();
    const TerrainPageRecord& record = manifest.GetPageRecords()[pageIndex];
    const TerrainCookStatus status = ValidatePageBlob(layout, record, pageBytes);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    const BorrowedView<const uint8> payload = GetPayload(pageBytes);
    const usize coreSamples = static_cast<usize>(layout.CellsPerPage) + 1;
    const usize coreBytes = coreSamples * coreSamples * GetCodeBytes(layout.Codec.Encoding);
    TerrainPage candidate;
    const TerrainTileStatus tileStatus = TryLoadTerrainTile(MakePageDescription(layout, record.Address),
                                                            {payload.GetDataOrNull(), coreBytes},
                                                            TerrainPageAccess::GetTile(candidate));
    if (tileStatus != TerrainTileStatus::Success)
    {
        return GetCookStatus(tileStatus);
    }
    if (TerrainPageAccess::GetHaloCodes(candidate).TryAddRange(
            {payload.GetDataOrNull() + coreBytes, payload.GetSize() - coreBytes}) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    TerrainPageAccess::GetRecord(candidate) = record;
    outPage = foundation::Move(candidate);
    return TerrainCookStatus::Success;
}

TerrainCookStatus
TryGetTerrainPageHeight(const TerrainPage& page, int32 column, int32 row, float64& outHeightMetres) noexcept
{
    if (page.IsEmpty() == true)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    const TerrainTileDescription description = page.GetTile().GetDescription();
    const int64 cells = description.CellsPerSide;
    if (column < -HALO || column > cells + HALO || row < -HALO || row > cells + HALO)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    if (column >= 0 && column <= cells && row >= 0 && row <= cells)
    {
        outHeightMetres =
            internal::DecodeTerrainTileHeight(page.GetTile(), static_cast<uint32>(column), static_cast<uint32>(row));
        return TerrainCookStatus::Success;
    }
    const usize coreSamples = static_cast<usize>(cells) + 1;
    const usize haloIndex =
        GetPayloadSampleIndex(description.CellsPerSide, {column, row}) - (coreSamples * coreSamples);
    const int64 code = ReadCode(TerrainPageAccess::GetHaloCodes(page), description.Codec.Encoding, haloIndex);
    outHeightMetres = internal::DecodeTerrainHeight(description.Codec, code);
    return TerrainCookStatus::Success;
}

TerrainCookStatus TryCollectTerrainEditPages(const TerrainRevisionLayout& layout,
                                             TerrainSampleRegion region,
                                             BorrowedView<uint32> outPageIndices,
                                             usize& outCount) noexcept
{
    const TerrainCookStatus status = ValidateLayout(layout);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    if (region.Minimum.X > region.Maximum.X || region.Minimum.Z > region.Maximum.Z)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    if (outPageIndices.IsValid() == false)
    {
        return TerrainCookStatus::InvalidStorage;
    }
    // Clip to the domain first: halo samples outside it repeat domain samples, so only domain samples matter,
    // and the clipped bounds fit domain-relative coordinates.
    const TerrainSampleAddress origin = GetDomainOrigin(layout);
    const int64 lastColumn = static_cast<int64>(GetDomainColumns(layout)) - 1;
    const int64 lastRow = static_cast<int64>(GetDomainRows(layout)) - 1;
    const int64 endX = origin.X + lastColumn;
    const int64 endZ = origin.Z + lastRow;
    if (DoesRangeIntersect(region.Minimum.X, region.Maximum.X, origin.X, endX) == false ||
        DoesRangeIntersect(region.Minimum.Z, region.Maximum.Z, origin.Z, endZ) == false)
    {
        outCount = 0;
        return TerrainCookStatus::Success;
    }
    const int64 minimumColumn = (region.Minimum.X > origin.X ? region.Minimum.X : origin.X) - origin.X;
    const int64 maximumColumn = (region.Maximum.X < endX ? region.Maximum.X : endX) - origin.X;
    const int64 minimumRow = (region.Minimum.Z > origin.Z ? region.Minimum.Z : origin.Z) - origin.Z;
    const int64 maximumRow = (region.Maximum.Z < endZ ? region.Maximum.Z : endZ) - origin.Z;
    const int64 cells = layout.CellsPerPage;

    usize count = 0;
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        const bool shouldWrite = pass == 1;
        if (shouldWrite == true && count > outPageIndices.GetSize())
        {
            return TerrainCookStatus::LimitExceeded;
        }
        count = 0;
        for (usize i = 0; i < GetPageCount(layout); ++i)
        {
            const int64 pageColumn = static_cast<int64>(i % layout.PagesX) * cells;
            const int64 pageRow = static_cast<int64>(i / layout.PagesX) * cells;
            if (DoesRangeIntersect(pageColumn - HALO, pageColumn + cells + HALO, minimumColumn, maximumColumn) ==
                    true &&
                DoesRangeIntersect(pageRow - HALO, pageRow + cells + HALO, minimumRow, maximumRow) == true)
            {
                if (shouldWrite == true)
                {
                    outPageIndices[count] = static_cast<uint32>(i);
                }
                ++count;
            }
        }
    }
    outCount = count;
    return TerrainCookStatus::Success;
}

TerrainCookStatus TryExtractTerrainCookedPage(const TerrainManifest& manifest,
                                              BorrowedView<const uint8> pagePackBytes,
                                              usize pageIndex,
                                              TerrainCookedPage& outPage) noexcept
{
    const TerrainCookStatus status = ValidatePagePack(manifest, pagePackBytes);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    if (pageIndex >= manifest.GetPageRecords().GetSize())
    {
        return TerrainCookStatus::InvalidArgument;
    }
    return ExtractPage(manifest, pagePackBytes, pageIndex, outPage);
}

TerrainCookStatus TryRecookTerrainRevision(const TerrainManifest& baseManifest,
                                           BorrowedView<const uint8> basePagePackBytes,
                                           const TerrainRevisionLayout& layout,
                                           const TerrainFieldGrid& grid,
                                           BorrowedView<const float32> elevationsMetres,
                                           TerrainSampleRegion region,
                                           TerrainCookedRevision& outRevision,
                                           TerrainRecookSummary& outSummary) noexcept
{
    TerrainCookStatus status = ValidatePagePack(baseManifest, basePagePackBytes);
    if (status == TerrainCookStatus::Success)
    {
        status = ValidateLayout(layout);
    }
    if (status == TerrainCookStatus::Success)
    {
        status = ValidateField(layout, grid, elevationsMetres);
    }
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    if (AreLayoutsCompatible(baseManifest.GetLayout(), layout) == false ||
        layout.Revision <= baseManifest.GetLayout().Revision)
    {
        return TerrainCookStatus::InvalidArgument;
    }
    const usize pageCount = GetPageCount(layout);
    Array<uint32> editedPages;
    Array<TerrainCookedPage> pages;
    if (editedPages.TryResize(pageCount) == false || pages.TryResize(pageCount) == false)
    {
        return TerrainCookStatus::AllocationFailed;
    }
    usize editedCount = 0;
    status = TryCollectTerrainEditPages(layout, region, editedPages.AsView(), editedCount);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    usize nextEdited = 0;
    for (usize i = 0; i < pageCount; ++i)
    {
        const bool isEdited = nextEdited < editedCount && editedPages[nextEdited] == i;
        if (isEdited == true)
        {
            ++nextEdited;
            status = CookPage(layout, grid, elevationsMetres, i, pages[i]);
        }
        else
        {
            status = ExtractPage(baseManifest, basePagePackBytes, i, pages[i]);
        }
        if (status != TerrainCookStatus::Success)
        {
            return status;
        }
    }
    status = TryAssembleTerrainRevision(layout, pages.AsView(), outRevision);
    if (status != TerrainCookStatus::Success)
    {
        return status;
    }
    outSummary = {editedCount, pageCount - editedCount};
    return TerrainCookStatus::Success;
}
} // namespace ludus::content::terrain
