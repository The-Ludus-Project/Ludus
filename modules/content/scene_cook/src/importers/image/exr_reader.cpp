// Thanks to Industrial Light & Magic and the OpenEXR project authors for
// "OpenEXR File Layout" (https://openexr.com/en/latest/OpenEXRFileLayout.html)
// and "Technical Introduction to OpenEXR"
// (https://openexr.com/en/latest/TechnicalIntroduction.html): the header,
// channel list, offset table and chunk layouts, and the RLE/ZIP byte predictor
// and interleaving read here. This is an original bounded subset reader, not
// OpenEXR library code; zlib streams use the vendored stb_image inflater by
// Sean Barrett (third_party/stb).

#include "exr_reader.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <bit>

extern "C" {
#include "stb_image.h"
#include "stb_image_config.h"
}

namespace ludus::content::scene_cook::importers
{
namespace
{
using foundation::core::int32;
using foundation::core::int64;
using foundation::core::uint16;
using foundation::core::uint64;

/// Magic number 20000630 as its four little-endian file bytes.
constexpr usize EXR_MAGIC_BYTES = 4;
constexpr foundation::StaticArray<uint8, EXR_MAGIC_BYTES> EXR_MAGIC{0x76, 0x2f, 0x31, 0x01};
/// Width of int, unsigned int and float header fields (and the version field).
constexpr usize EXR_INT_BYTES = 4;
/// Width of an offset-table entry (unsigned long).
constexpr usize EXR_OFFSET_BYTES = 8;
/// box2i attribute: xMin, yMin, xMax and yMax ints.
constexpr usize EXR_BOX2I_BYTES = 4 * EXR_INT_BYTES;
/// tiledesc attribute: xSize and ySize unsigned ints, then the level/rounding mode byte.
constexpr usize EXR_TILEDESC_BYTES = 2 * EXR_INT_BYTES + 1;
constexpr usize BITS_PER_BYTE = 8;
/// Version field: format version 2 in the low byte, feature flags above it.
constexpr uint32 EXR_FORMAT_VERSION = 2;
constexpr uint32 EXR_VERSION_NUMBER_MASK = 0xff;
constexpr uint32 EXR_FLAG_TILED = 0x200;
constexpr uint32 EXR_FLAG_LONG_NAMES = 0x400;
constexpr uint32 EXR_FLAG_DEEP = 0x800;
constexpr uint32 EXR_FLAG_MULTIPART = 0x1000;
constexpr uint32 EXR_KNOWN_FLAGS = EXR_FLAG_TILED | EXR_FLAG_LONG_NAMES | EXR_FLAG_DEEP | EXR_FLAG_MULTIPART;
/// Attribute and channel names: 31 bytes, or 255 with the long-names flag.
constexpr usize SHORT_NAME_BYTES = 31;
constexpr usize LONG_NAME_BYTES = 255;
constexpr usize MAX_CHANNELS = 64;
constexpr uint32 PIXEL_UINT = 0;
constexpr uint32 PIXEL_HALF = 1;
constexpr uint32 PIXEL_FLOAT = 2;
constexpr uint8 COMPRESSION_NONE = 0;
constexpr uint8 COMPRESSION_RLE = 1;
constexpr uint8 COMPRESSION_ZIPS = 2;
constexpr uint8 COMPRESSION_ZIP = 3;
/// Scanlines per chunk: ZIP compresses 16 lines together; NONE, RLE and ZIPS one.
constexpr uint32 ZIP_LINES_PER_BLOCK = 16;
/// The RLE/ZIP predictor stores each byte as (difference + 128) modulo 256.
constexpr int32 PREDICTOR_BIAS = 128;
/// RLE run headers are two's-complement bytes: values from SIGNED_BYTE_LIMIT up wrap to
/// negative literal counts by subtracting BYTE_VALUE_COUNT.
constexpr int32 SIGNED_BYTE_LIMIT = 128;
constexpr int32 BYTE_VALUE_COUNT = 256;
/// Bytes per sample: HALF is two, UINT and FLOAT are four.
constexpr usize HALF_SAMPLE_BYTES = 2;
constexpr usize WORD_SAMPLE_BYTES = 4;
/// IEEE 754 binary16 (sign 1, exponent 5, mantissa 10 bits) widened to binary32
/// (exponent 8, mantissa 23 bits); exponent biases 15 and 127.
constexpr uint32 HALF_SIGN_MASK = 0x8000;
constexpr uint32 HALF_MANTISSA_BITS = 10;
constexpr uint32 HALF_MANTISSA_MASK = 0x3ff;
constexpr uint32 HALF_IMPLICIT_BIT = 0x400;
constexpr uint32 HALF_EXPONENT_MASK = 0x1f;
constexpr uint32 HALF_EXPONENT_BIAS = 15;
constexpr uint32 FLOAT_MANTISSA_BITS = 23;
constexpr uint32 FLOAT_EXPONENT_BIAS = 127;
constexpr uint32 FLOAT_INFINITY_BITS = 0x7f800000;
/// Moves the half sign bit (15) to the binary32 sign bit (31).
constexpr uint32 HALF_TO_FLOAT_SIGN_SHIFT = 16;
constexpr uint32 HALF_TO_FLOAT_MANTISSA_SHIFT = FLOAT_MANTISSA_BITS - HALF_MANTISSA_BITS;
constexpr uint32 HALF_TO_FLOAT_EXPONENT_REBIAS = FLOAT_EXPONENT_BIAS - HALF_EXPONENT_BIAS;
/// Largest uncompressed chunk admitted (the inflater takes int sizes).
constexpr usize MAX_CHUNK_BYTES = usize{256} * 1024 * 1024;

struct ExrChannel final
{
    std::string_view Name;
    uint32 PixelType = PIXEL_HALF;
    usize SampleBytes = HALF_SAMPLE_BYTES;
};

struct ExrHeader final
{
    foundation::StaticArray<ExrChannel, MAX_CHANNELS> Channels{};
    usize ChannelCount = 0;
    uint8 Compression = COMPRESSION_NONE;
    int32 MinimumX = 0;
    int32 MinimumY = 0;
    int32 MaximumX = -1;
    int32 MaximumY = -1;
    uint32 TileWidth = 0;
    uint32 TileHeight = 0;
    bool HasChannels = false;
    bool HasCompression = false;
    bool HasDataWindow = false;
    bool HasTiles = false;
    bool IsTiled = false;
};

/// Little-endian cursor that fails (and stays failed) instead of reading past the end.
class ByteCursor final
{
public:
    ByteCursor(std::span<const uint8> data, usize offset) noexcept : mData(data), mOffset(offset) {}
    [[nodiscard]] LUDUS_INLINE bool IsOk() const noexcept
    {
        return mIsOk;
    }
    [[nodiscard]] LUDUS_INLINE usize GetOffset() const noexcept
    {
        return mOffset;
    }
    [[nodiscard]] uint64 ReadUnsigned(usize byteCount) noexcept
    {
        if (mIsOk == false || mData.size() - mOffset < byteCount)
        {
            mIsOk = false;
            return 0;
        }
        uint64 value = 0;
        for (usize i = 0; i < byteCount; ++i)
        {
            value |= static_cast<uint64>(mData[mOffset + i]) << (BITS_PER_BYTE * i);
        }
        mOffset += byteCount;
        return value;
    }
    [[nodiscard]] int32 ReadInt32() noexcept
    {
        return std::bit_cast<int32>(static_cast<uint32>(ReadUnsigned(EXR_INT_BYTES)));
    }
    /// NUL-terminated name of at most maximumBytes bytes (the terminator excluded).
    [[nodiscard]] std::string_view ReadName(usize maximumBytes) noexcept
    {
        const usize start = mOffset;
        while (mIsOk == true && mOffset < mData.size() && mData[mOffset] != 0)
        {
            ++mOffset;
        }
        if (mIsOk == false || mOffset >= mData.size() || mOffset - start > maximumBytes)
        {
            mIsOk = false;
            return {};
        }
        const std::string_view name(reinterpret_cast<const char*>(mData.data() + start), mOffset - start);
        ++mOffset;
        return name;
    }
    [[nodiscard]] std::span<const uint8> ReadBytes(usize byteCount) noexcept
    {
        if (mIsOk == false || mData.size() - mOffset < byteCount)
        {
            mIsOk = false;
            return {};
        }
        const std::span<const uint8> bytes = mData.subspan(mOffset, byteCount);
        mOffset += byteCount;
        return bytes;
    }

private:
    std::span<const uint8> mData;
    usize mOffset = 0;
    bool mIsOk = true;
};

[[nodiscard]] float32 DecodeHalf(uint16 bits) noexcept
{
    const uint32 sign = (bits & HALF_SIGN_MASK) << HALF_TO_FLOAT_SIGN_SHIFT;
    const uint32 exponent = (uint32{bits} >> HALF_MANTISSA_BITS) & HALF_EXPONENT_MASK;
    uint32 mantissa = bits & HALF_MANTISSA_MASK;
    uint32 result = sign;
    if (exponent == 0)
    {
        if (mantissa != 0)
        {
            // Subnormal half: normalise into the binary32 exponent range.
            uint32 shift = 0;
            while ((mantissa & HALF_IMPLICIT_BIT) == 0)
            {
                mantissa <<= 1;
                ++shift;
            }
            result = sign | ((HALF_TO_FLOAT_EXPONENT_REBIAS + 1 - shift) << FLOAT_MANTISSA_BITS) |
                     ((mantissa & HALF_MANTISSA_MASK) << HALF_TO_FLOAT_MANTISSA_SHIFT);
        }
    }
    else if (exponent == HALF_EXPONENT_MASK)
    {
        result = sign | FLOAT_INFINITY_BITS | (mantissa << HALF_TO_FLOAT_MANTISSA_SHIFT);
    }
    else
    {
        result = sign | ((exponent + HALF_TO_FLOAT_EXPONENT_REBIAS) << FLOAT_MANTISSA_BITS) |
                 (mantissa << HALF_TO_FLOAT_MANTISSA_SHIFT);
    }
    return std::bit_cast<float32>(result);
}

[[nodiscard]] bool ParseChannels(std::span<const uint8> value, usize nameBytes, ExrHeader& inoutHeader) noexcept
{
    ByteCursor cursor(value, 0);
    inoutHeader.ChannelCount = 0;
    while (true)
    {
        const std::string_view name = cursor.ReadName(nameBytes);
        if (cursor.IsOk() == false)
        {
            return false;
        }
        if (name.empty() == true)
        {
            break;
        }
        const uint32 pixelType = static_cast<uint32>(cursor.ReadUnsigned(EXR_INT_BYTES));
        (void)cursor.ReadUnsigned(EXR_INT_BYTES); // pLinear and three reserved bytes.
        const int32 samplingX = cursor.ReadInt32();
        const int32 samplingY = cursor.ReadInt32();
        if (cursor.IsOk() == false || pixelType > PIXEL_FLOAT || inoutHeader.ChannelCount == MAX_CHANNELS ||
            samplingX != 1 || samplingY != 1)
        {
            return false;
        }
        ExrChannel& channel = inoutHeader.Channels[inoutHeader.ChannelCount++];
        channel.Name = name;
        channel.PixelType = pixelType;
        channel.SampleBytes = pixelType == PIXEL_HALF ? HALF_SAMPLE_BYTES : WORD_SAMPLE_BYTES;
    }
    return inoutHeader.ChannelCount != 0;
}

[[nodiscard]] ImageResult
ParseHeader(ByteCursor& inoutCursor, usize nameBytes, ExrHeader& inoutHeader, std::string_view& outMessage) noexcept
{
    while (true)
    {
        const std::string_view name = inoutCursor.ReadName(nameBytes);
        if (inoutCursor.IsOk() == false)
        {
            outMessage = "EXR header is truncated";
            return ImageResult::Invalid;
        }
        if (name.empty() == true)
        {
            return ImageResult::Ok;
        }
        const std::string_view type = inoutCursor.ReadName(nameBytes);
        const int32 size = inoutCursor.ReadInt32();
        const std::span<const uint8> value = inoutCursor.ReadBytes(size < 0 ? usize{0} : static_cast<usize>(size));
        if (inoutCursor.IsOk() == false || size < 0)
        {
            outMessage = "EXR header attribute is truncated";
            return ImageResult::Invalid;
        }
        if (name == "channels" && type == "chlist")
        {
            if (ParseChannels(value, nameBytes, inoutHeader) == false)
            {
                outMessage = "EXR channel list is malformed or uses subsampled channels";
                return ImageResult::Unsupported;
            }
            inoutHeader.HasChannels = true;
        }
        else if (name == "compression" && type == "compression" && value.size() == 1)
        {
            inoutHeader.Compression = value[0];
            inoutHeader.HasCompression = true;
        }
        else if (name == "dataWindow" && type == "box2i" && value.size() == EXR_BOX2I_BYTES)
        {
            ByteCursor box(value, 0);
            inoutHeader.MinimumX = box.ReadInt32();
            inoutHeader.MinimumY = box.ReadInt32();
            inoutHeader.MaximumX = box.ReadInt32();
            inoutHeader.MaximumY = box.ReadInt32();
            inoutHeader.HasDataWindow = true;
        }
        else if (name == "tiles" && type == "tiledesc" && value.size() == EXR_TILEDESC_BYTES)
        {
            ByteCursor tiles(value, 0);
            inoutHeader.TileWidth = static_cast<uint32>(tiles.ReadUnsigned(EXR_INT_BYTES));
            inoutHeader.TileHeight = static_cast<uint32>(tiles.ReadUnsigned(EXR_INT_BYTES));
            inoutHeader.HasTiles = true;
        }
    }
}

[[nodiscard]] uint32 GetLinesPerBlock(uint8 compression) noexcept
{
    return compression == COMPRESSION_ZIP ? ZIP_LINES_PER_BLOCK : 1U;
}

/// Undo the RLE/ZIP byte predictor and the two-half interleaving into outBytes.
void ReconstructBytes(Array<uint8>& inoutScratch, usize size, uint8* outBytes) noexcept
{
    uint8* bytes = inoutScratch.GetData();
    for (usize i = 1; i < size; ++i)
    {
        bytes[i] = static_cast<uint8>(bytes[i - 1] + bytes[i] - PREDICTOR_BIAS);
    }
    const uint8* first = bytes;
    const uint8* second = bytes + (size + 1) / 2;
    for (usize i = 0; i < size; ++i)
    {
        outBytes[i] = (i % 2 == 0) ? *first++ : *second++;
    }
}

/// Expand one compressed chunk into exactly expectedSize bytes.
[[nodiscard]] ImageResult DecompressChunk(uint8 compression,
                                          std::span<const uint8> packed,
                                          usize expectedSize,
                                          Array<uint8>& inoutScratch,
                                          Array<uint8>& outBlock,
                                          std::string_view& outMessage) noexcept
{
    if (outBlock.TryResize(expectedSize) == false || inoutScratch.TryResize(expectedSize) == false)
    {
        return ImageResult::OutOfMemory;
    }
    // A chunk that would not shrink is stored raw whatever the compression.
    if (packed.size() == expectedSize)
    {
        for (usize i = 0; i < expectedSize; ++i)
        {
            outBlock[i] = packed[i];
        }
        return ImageResult::Ok;
    }
    if (compression == COMPRESSION_NONE)
    {
        outMessage = "EXR uncompressed chunk has the wrong size";
        return ImageResult::Invalid;
    }
    if (compression == COMPRESSION_RLE)
    {
        usize written = 0;
        usize at = 0;
        while (at < packed.size())
        {
            // The run header is a signed byte: negative counts literal bytes.
            const int32 header = packed[at++];
            const int32 count = header < SIGNED_BYTE_LIMIT ? header : header - BYTE_VALUE_COUNT;
            if (count < 0)
            {
                const usize literal = static_cast<usize>(-count);
                if (packed.size() - at < literal || expectedSize - written < literal)
                {
                    outMessage = "EXR RLE chunk is corrupt";
                    return ImageResult::Invalid;
                }
                for (usize i = 0; i < literal; ++i)
                {
                    inoutScratch[written++] = packed[at++];
                }
            }
            else
            {
                const usize run = static_cast<usize>(count) + 1;
                if (at >= packed.size() || expectedSize - written < run)
                {
                    outMessage = "EXR RLE chunk is corrupt";
                    return ImageResult::Invalid;
                }
                for (usize i = 0; i < run; ++i)
                {
                    inoutScratch[written++] = packed[at];
                }
                ++at;
            }
        }
        if (written != expectedSize)
        {
            outMessage = "EXR RLE chunk is truncated";
            return ImageResult::Invalid;
        }
    }
    else if (compression == COMPRESSION_ZIPS || compression == COMPRESSION_ZIP)
    {
        const int decoded = stbi_zlib_decode_buffer(reinterpret_cast<char*>(inoutScratch.GetData()),
                                                    static_cast<int>(expectedSize),
                                                    reinterpret_cast<const char*>(packed.data()),
                                                    static_cast<int>(packed.size()));
        if (decoded < 0 || static_cast<usize>(decoded) != expectedSize)
        {
            outMessage = "EXR ZIP chunk is corrupt";
            return ImageResult::Invalid;
        }
    }
    else
    {
        outMessage = "EXR compression is not supported (NONE, RLE, ZIPS and ZIP are)";
        return ImageResult::Unsupported;
    }
    ReconstructBytes(inoutScratch, expectedSize, outBlock.GetData());
    return ImageResult::Ok;
}

[[nodiscard]] uint32 LoadLittleEndian(const uint8* bytes, usize byteCount) noexcept
{
    uint32 value = 0;
    for (usize i = 0; i < byteCount; ++i)
    {
        value |= static_cast<uint32>(bytes[i]) << (BITS_PER_BYTE * i);
    }
    return value;
}

[[nodiscard]] float32 ReadSample(const uint8* bytes, uint32 pixelType) noexcept
{
    if (pixelType == PIXEL_HALF)
    {
        return DecodeHalf(static_cast<uint16>(LoadLittleEndian(bytes, HALF_SAMPLE_BYTES)));
    }
    const uint32 bits = LoadLittleEndian(bytes, WORD_SAMPLE_BYTES);
    return pixelType == PIXEL_UINT ? static_cast<float32>(bits) : std::bit_cast<float32>(bits);
}

/// Region of the data window covered by one decoded chunk (zero-based, exclusive ends).
struct BlockRegion final
{
    uint32 FirstColumn = 0;
    uint32 EndColumn = 0;
    uint32 FirstRow = 0;
    uint32 EndRow = 0;
};

/// Scatter one decoded block (lines, then channels, then samples) into the RGB output.
void StoreBlock(const ExrHeader& header,
                const foundation::StaticArray<usize, LIGHTING_IMAGE_CHANNEL_COUNT>& sources,
                const BlockRegion& region,
                const Array<uint8>& block,
                uint32 width,
                Array<float32>& inoutRgb) noexcept
{
    const usize columns = region.EndColumn - region.FirstColumn;
    usize at = 0;
    for (uint32 row = region.FirstRow; row < region.EndRow; ++row)
    {
        for (usize channel = 0; channel < header.ChannelCount; ++channel)
        {
            const ExrChannel& description = header.Channels[channel];
            for (usize k = 0; k < LIGHTING_IMAGE_CHANNEL_COUNT; ++k)
            {
                if (sources[k] != channel)
                {
                    continue;
                }
                for (usize column = 0; column < columns; ++column)
                {
                    const usize target =
                        (usize{row} * width + region.FirstColumn + column) * LIGHTING_IMAGE_CHANNEL_COUNT + k;
                    inoutRgb[target] =
                        ReadSample(block.GetData() + at + column * description.SampleBytes, description.PixelType);
                }
            }
            at += columns * description.SampleBytes;
        }
    }
}
} // namespace

bool HasExrMagic(std::span<const uint8> data) noexcept
{
    if (data.size() < EXR_MAGIC_BYTES)
    {
        return false;
    }
    for (usize i = 0; i < EXR_MAGIC_BYTES; ++i)
    {
        if (data[i] != EXR_MAGIC[i])
        {
            return false;
        }
    }
    return true;
}

ImageResult
DecodeExr(std::span<const uint8> data, DecodedLightingImage& outImage, std::string_view& outMessage) noexcept
{
    if (HasExrMagic(data) == false || data.size() < EXR_MAGIC_BYTES + EXR_INT_BYTES)
    {
        outMessage = "not an OpenEXR file";
        return ImageResult::Invalid;
    }
    ByteCursor cursor(data, EXR_MAGIC_BYTES);
    const uint32 version = static_cast<uint32>(cursor.ReadUnsigned(EXR_INT_BYTES));
    if ((version & EXR_VERSION_NUMBER_MASK) != EXR_FORMAT_VERSION ||
        (version & ~(EXR_VERSION_NUMBER_MASK | EXR_KNOWN_FLAGS)) != 0)
    {
        outMessage = "unsupported OpenEXR version";
        return ImageResult::Unsupported;
    }
    if ((version & (EXR_FLAG_DEEP | EXR_FLAG_MULTIPART)) != 0)
    {
        outMessage = "deep or multi-part OpenEXR files are not supported";
        return ImageResult::Unsupported;
    }
    ExrHeader header;
    header.IsTiled = (version & EXR_FLAG_TILED) != 0;
    const usize nameBytes = (version & EXR_FLAG_LONG_NAMES) != 0 ? LONG_NAME_BYTES : SHORT_NAME_BYTES;
    const ImageResult parsed = ParseHeader(cursor, nameBytes, header, outMessage);
    if (parsed != ImageResult::Ok)
    {
        return parsed;
    }
    if (header.HasChannels == false || header.HasCompression == false || header.HasDataWindow == false ||
        (header.IsTiled == true && header.HasTiles == false))
    {
        outMessage = "EXR header lacks a required attribute";
        return ImageResult::Invalid;
    }
    if (header.Compression > COMPRESSION_ZIP)
    {
        outMessage = "EXR compression is not supported (NONE, RLE, ZIPS and ZIP are)";
        return ImageResult::Unsupported;
    }
    const int64 width64 = static_cast<int64>(header.MaximumX) - header.MinimumX + 1;
    const int64 height64 = static_cast<int64>(header.MaximumY) - header.MinimumY + 1;
    if (width64 < 1 || height64 < 1 || width64 > MAX_IMAGE_DIMENSION || height64 > MAX_IMAGE_DIMENSION ||
        static_cast<usize>(width64) * static_cast<usize>(height64) > MAX_DECODED_IMAGE_PIXELS)
    {
        outMessage = "EXR data window is empty or exceeds the image limits";
        return ImageResult::Invalid;
    }
    const uint32 width = static_cast<uint32>(width64);
    const uint32 height = static_cast<uint32>(height64);
    // R, G and B by name, or one Y channel for all three.
    foundation::StaticArray<usize, LIGHTING_IMAGE_CHANNEL_COUNT> sources{MAX_CHANNELS, MAX_CHANNELS, MAX_CHANNELS};
    usize pixelBytes = 0;
    for (usize i = 0; i < header.ChannelCount; ++i)
    {
        const std::string_view name = header.Channels[i].Name;
        pixelBytes += header.Channels[i].SampleBytes;
        if (name == "R" || name == "G" || name == "B")
        {
            sources[name == "R" ? 0 : name == "G" ? 1 : 2] = i;
        }
    }
    if (sources[0] == MAX_CHANNELS || sources[1] == MAX_CHANNELS || sources[2] == MAX_CHANNELS)
    {
        for (usize i = 0; i < header.ChannelCount; ++i)
        {
            if (header.Channels[i].Name == "Y")
            {
                sources = {i, i, i};
            }
        }
    }
    if (sources[0] == MAX_CHANNELS || sources[1] == MAX_CHANNELS || sources[2] == MAX_CHANNELS)
    {
        outMessage = "EXR has no R/G/B or Y channels";
        return ImageResult::Unsupported;
    }
    const uint32 blockWidth = header.IsTiled == true ? header.TileWidth : width;
    const uint32 blockHeight = header.IsTiled == true ? header.TileHeight : GetLinesPerBlock(header.Compression);
    if (blockWidth == 0 || blockHeight == 0 || blockWidth > MAX_IMAGE_DIMENSION || blockHeight > MAX_IMAGE_DIMENSION ||
        usize{blockWidth} * blockHeight * pixelBytes > MAX_CHUNK_BYTES)
    {
        outMessage = "EXR tile or block size is out of range";
        return ImageResult::Invalid;
    }
    const usize columnsOfBlocks = header.IsTiled == true ? (usize{width} + blockWidth - 1) / blockWidth : 1;
    const usize rowsOfBlocks = (usize{height} + blockHeight - 1) / blockHeight;
    const usize chunkCount = columnsOfBlocks * rowsOfBlocks;
    // Level-0 tiles come first in the offset table of mipmapped/ripmapped files.
    const usize tableOffset = cursor.GetOffset();
    if ((data.size() - tableOffset) / EXR_OFFSET_BYTES < chunkCount)
    {
        outMessage = "EXR offset table is truncated";
        return ImageResult::Invalid;
    }
    DecodedLightingImage candidate;
    candidate.Width = width;
    candidate.Height = height;
    if (candidate.Rgb.TryResize(usize{width} * height * LIGHTING_IMAGE_CHANNEL_COUNT, 0.0F) == false)
    {
        return ImageResult::OutOfMemory;
    }
    Array<uint8> scratch;
    Array<uint8> block;
    for (usize chunk = 0; chunk < chunkCount; ++chunk)
    {
        ByteCursor table(data, tableOffset + chunk * EXR_OFFSET_BYTES);
        const uint64 offset = table.ReadUnsigned(EXR_OFFSET_BYTES);
        if (offset < tableOffset + chunkCount * EXR_OFFSET_BYTES || offset >= data.size())
        {
            outMessage = "EXR chunk offset is out of range";
            return ImageResult::Invalid;
        }
        ByteCursor chunkCursor(data, static_cast<usize>(offset));
        BlockRegion region;
        if (header.IsTiled == true)
        {
            const int32 tileX = chunkCursor.ReadInt32();
            const int32 tileY = chunkCursor.ReadInt32();
            const int32 levelX = chunkCursor.ReadInt32();
            const int32 levelY = chunkCursor.ReadInt32();
            if (tileX < 0 || tileY < 0 || static_cast<usize>(tileX) >= columnsOfBlocks ||
                static_cast<usize>(tileY) >= rowsOfBlocks || levelX != 0 || levelY != 0)
            {
                outMessage = "EXR tile coordinates are out of range";
                return ImageResult::Invalid;
            }
            region.FirstColumn = static_cast<uint32>(tileX) * blockWidth;
            region.FirstRow = static_cast<uint32>(tileY) * blockHeight;
        }
        else
        {
            const int64 lineY = static_cast<int64>(chunkCursor.ReadInt32()) - header.MinimumY;
            if (lineY < 0 || lineY >= height64 || lineY % blockHeight != 0)
            {
                outMessage = "EXR scanline block is out of range";
                return ImageResult::Invalid;
            }
            region.FirstRow = static_cast<uint32>(lineY);
        }
        region.EndColumn = region.FirstColumn + blockWidth < width ? region.FirstColumn + blockWidth : width;
        region.EndRow = region.FirstRow + blockHeight < height ? region.FirstRow + blockHeight : height;
        const int32 packedSize = chunkCursor.ReadInt32();
        const std::span<const uint8> packed =
            chunkCursor.ReadBytes(packedSize < 0 ? usize{0} : static_cast<usize>(packedSize));
        if (chunkCursor.IsOk() == false || packedSize < 0)
        {
            outMessage = "EXR chunk is truncated";
            return ImageResult::Invalid;
        }
        const usize expectedSize =
            usize{region.EndColumn - region.FirstColumn} * (region.EndRow - region.FirstRow) * pixelBytes;
        const ImageResult expanded =
            DecompressChunk(header.Compression, packed, expectedSize, scratch, block, outMessage);
        if (expanded != ImageResult::Ok)
        {
            return expanded;
        }
        StoreBlock(header, sources, region, block, width, candidate.Rgb);
    }
    outImage = static_cast<DecodedLightingImage&&>(candidate);
    return ImageResult::Ok;
}
} // namespace ludus::content::scene_cook::importers
