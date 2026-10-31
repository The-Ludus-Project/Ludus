// TGA layout: Truevision Inc., "Truevision TGA File Format Specification",
// version 2.0 (1991). PFM layout: Paul Debevec's Portable Float Map notes,
// https://www.pauldebevec.com/Research/HDR/PFM/. PNG, JPEG (baseline and
// progressive) and Radiance RGBE decoding use stb_image by Sean Barrett
// (vendored in third_party/stb, behind its bounded allocator); OpenEXR uses
// the private subset reader in exr_reader.cpp.

#include "image_reader.h"

#include "exr_reader.h"

#include "../decimal_number.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <bit>
#include <cstring>

extern "C" {
#include "stb_image.h"
#include "stb_image_config.h"
}

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace math = foundation::math;
using foundation::core::float32;
using foundation::core::float64;
using foundation::core::uint16;

/// Index of the alpha channel in an RGBA8 texel.
constexpr usize ALPHA_CHANNEL_INDEX = 3;
constexpr uint8 OPAQUE_ALPHA = 255;
/// Bytes inspected for a Radiance "#?RADIANCE"/"#?RGBE" program-type signature.
constexpr usize RADIANCE_SIGNATURE_SCAN_BYTES = 16;
/// JPEG start-of-image marker (FF D8) followed by the next marker's FF prefix.
constexpr usize JPEG_SIGNATURE_BYTES = 3;
constexpr foundation::StaticArray<uint8, JPEG_SIGNATURE_BYTES> JPEG_SIGNATURE{0xff, 0xd8, 0xff};
/// stb_image takes the input length as an int.
constexpr usize MAX_STB_INPUT_BYTES = 0x7fffffff;
/// Each 2x2 box-filter tap weighs one quarter.
constexpr float64 BOX_FILTER_TAP_WEIGHT = 0.25;
/// PFM header words: identifier ("PF" or "Pf"), width, height and scale.
constexpr usize PFM_IDENTIFIER_WORD = 0;
constexpr usize PFM_WIDTH_WORD = 1;
constexpr usize PFM_HEIGHT_WORD = 2;
constexpr usize PFM_SCALE_WORD = 3;
constexpr usize PFM_HEADER_WORD_COUNT = 4;

[[nodiscard]] bool HasExtension(std::string_view name, std::string_view extension) noexcept
{
    if (name.size() < extension.size())
    {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - extension.size());
    for (usize i = 0; i < extension.size(); ++i)
    {
        char value = tail[i];
        if (value >= 'A' && value <= 'Z')
        {
            value = static_cast<char>(value - 'A' + 'a');
        }
        if (value != extension[i])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AllocateImage(uint32 width, uint32 height, DecodedImage& outImage) noexcept
{
    outImage.Width = width;
    outImage.Height = height;
    return outImage.Texels.TryResize(usize{width} * height * 4) == true;
}

[[nodiscard]] uint16 ReadUint16(std::span<const uint8> data, usize at) noexcept
{
    return static_cast<uint16>(data[at] | (data[at + 1] << 8));
}

[[nodiscard]] ImageResult
DecodeTga(std::span<const uint8> data, DecodedImage& outImage, std::string_view& outMessage) noexcept
{
    if (data.size() < 18)
    {
        outMessage = "TGA header is truncated";
        return ImageResult::Invalid;
    }
    const uint8 idLength = data[0];
    const uint8 colourMapType = data[1];
    const uint8 imageType = data[2];
    const uint16 mapFirst = ReadUint16(data, 3);
    const uint16 mapLength = ReadUint16(data, 5);
    const uint8 mapDepth = data[7];
    const uint32 width = ReadUint16(data, 12);
    const uint32 height = ReadUint16(data, 14);
    const uint8 depth = data[16];
    const uint8 descriptor = data[17];
    const bool isRle = imageType >= 9;
    const uint8 baseType = isRle == true ? static_cast<uint8>(imageType - 8) : imageType;
    if ((baseType != 1 && baseType != 2 && baseType != 3) || width == 0 || height == 0)
    {
        outMessage = "unsupported TGA image type";
        return ImageResult::Invalid;
    }
    const bool isMapped = baseType == 1;
    if ((isMapped == true && (colourMapType != 1 || (depth != 8 && depth != 16) ||
                              (mapDepth != 15 && mapDepth != 16 && mapDepth != 24 && mapDepth != 32))) ||
        (baseType == 2 && depth != 24 && depth != 32 && depth != 16 && depth != 15) ||
        (baseType == 3 && depth != 8 && depth != 16))
    {
        outMessage = "unsupported TGA pixel depth";
        return ImageResult::Invalid;
    }
    usize at = 18 + idLength;
    const usize mapEntryBytes = (static_cast<usize>(mapDepth) + 7) / 8;
    const usize mapOffset = at;
    if (colourMapType == 1)
    {
        at += mapEntryBytes * mapLength;
    }
    if (at > data.size())
    {
        outMessage = "TGA is truncated";
        return ImageResult::Invalid;
    }
    if (AllocateImage(width, height, outImage) == false)
    {
        return ImageResult::OutOfMemory;
    }
    const usize pixelBytes = (static_cast<usize>(depth) + 7) / 8;
    // Convert one stored pixel (or colour-map entry) of the given depth to RGBA.
    const auto convert = [](const uint8* source, usize bits, bool isGrey, uint8(&outRgba)[4]) noexcept {
        if (isGrey == true)
        {
            outRgba[0] = outRgba[1] = outRgba[2] = source[0];
            outRgba[3] = bits == 16 ? source[1] : uint8{255};
            return;
        }
        if (bits == 15 || bits == 16)
        {
            const uint16 packed = static_cast<uint16>(source[0] | (source[1] << 8));
            outRgba[0] = static_cast<uint8>(((packed >> 10) & 31) * 255 / 31);
            outRgba[1] = static_cast<uint8>(((packed >> 5) & 31) * 255 / 31);
            outRgba[2] = static_cast<uint8>((packed & 31) * 255 / 31);
            outRgba[3] = 255;
            return;
        }
        outRgba[0] = source[2];
        outRgba[1] = source[1];
        outRgba[2] = source[0];
        outRgba[3] = bits == 32 ? source[3] : uint8{255};
    };
    const auto decodePixel = [&](const uint8* source, uint8(&outRgba)[4]) noexcept -> bool {
        if (isMapped == false)
        {
            convert(source, depth, baseType == 3, outRgba);
            return true;
        }
        const usize index = depth == 8 ? source[0] : static_cast<usize>(source[0] | (source[1] << 8));
        if (index < mapFirst || index - mapFirst >= mapLength)
        {
            return false;
        }
        convert(data.data() + mapOffset + (index - mapFirst) * mapEntryBytes, mapDepth, false, outRgba);
        return true;
    };
    const usize count = usize{width} * height;
    usize written = 0;
    const bool isTopDown = (descriptor & 0x20) != 0;
    const auto store = [&](const uint8(&rgba)[4]) noexcept {
        const usize x = written % width;
        const usize row = written / width;
        const usize y = isTopDown == true ? row : height - 1 - row;
        uint8* target = outImage.Texels.GetData() + (y * width + x) * 4;
        for (usize k = 0; k < 4; ++k)
        {
            target[k] = rgba[k];
        }
        ++written;
    };
    while (written < count)
    {
        usize run = 1;
        bool isRepeated = false;
        if (isRle == true)
        {
            if (at >= data.size())
            {
                break;
            }
            const uint8 header = data[at++];
            run = static_cast<usize>(header & 0x7f) + 1;
            isRepeated = (header & 0x80) != 0;
        }
        for (usize i = 0; i < run && written < count; ++i)
        {
            if (i == 0 || isRepeated == false)
            {
                if (at + pixelBytes > data.size())
                {
                    outMessage = "TGA pixel data is truncated";
                    return ImageResult::Invalid;
                }
            }
            uint8 rgba[4] = {0, 0, 0, 255};
            if (decodePixel(data.data() + at, rgba) == false)
            {
                outMessage = "TGA colour-map index out of range";
                return ImageResult::Invalid;
            }
            if (isRepeated == false)
            {
                at += pixelBytes;
            }
            store(rgba);
        }
        if (isRepeated == true)
        {
            at += pixelBytes;
        }
    }
    if (written != count)
    {
        outMessage = "TGA pixel data is truncated";
        return ImageResult::Invalid;
    }
    return ImageResult::Ok;
}

/// stb_image reports allocation failure (including its bounded allocator) as "outofmem".
[[nodiscard]] bool HasStbRunOutOfMemory() noexcept
{
    const char* reasonOrNull = stbi_failure_reason();
    return reasonOrNull != nullptr && std::string_view(reasonOrNull) == "outofmem";
}

/// Decode a PNG or JPEG through stb_image into RGBA8.
[[nodiscard]] ImageResult DecodeWithStb(std::span<const uint8> data,
                                        std::string_view failure,
                                        DecodedImage& outImage,
                                        std::string_view& outMessage) noexcept
{
    if (data.size() > MAX_STB_INPUT_BYTES)
    {
        outMessage = "image file is too large for the decoder";
        return ImageResult::Invalid;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    // Check the declared raster before decoding: a few bytes can declare a huge image.
    if (stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &channels) == 0)
    {
        outMessage = failure;
        return ImageResult::Invalid;
    }
    if (width <= 0 || height <= 0 || static_cast<usize>(width) * static_cast<usize>(height) > MAX_DECODED_IMAGE_PIXELS)
    {
        outMessage = "image dimensions exceed the decoded pixel limit";
        return ImageResult::Invalid;
    }
    stbi_uc* pixelsOrNull = stbi_load_from_memory(data.data(),
                                                  static_cast<int>(data.size()),
                                                  &width,
                                                  &height,
                                                  &channels,
                                                  static_cast<int>(DECODED_IMAGE_CHANNEL_COUNT));
    if (pixelsOrNull == nullptr)
    {
        if (HasStbRunOutOfMemory() == true)
        {
            return ImageResult::OutOfMemory;
        }
        outMessage = failure;
        return ImageResult::Invalid;
    }
    stbi_uc* pixels = pixelsOrNull;
    ImageResult result = ImageResult::Ok;
    if (width <= 0 || height <= 0 || static_cast<uint32>(width) > MAX_IMAGE_DIMENSION ||
        static_cast<uint32>(height) > MAX_IMAGE_DIMENSION)
    {
        outMessage = "image dimensions out of range";
        result = ImageResult::Invalid;
    }
    else
    {
        outImage.Width = static_cast<uint32>(width);
        outImage.Height = static_cast<uint32>(height);
        outImage.Texels.Clear();
        const usize byteCount = static_cast<usize>(width) * static_cast<usize>(height) * DECODED_IMAGE_CHANNEL_COUNT;
        if (outImage.Texels.TryAddRange(std::span<const uint8>(pixels, byteCount)) == false)
        {
            result = ImageResult::OutOfMemory;
        }
    }
    stbi_image_free(pixels);
    return result;
}

[[nodiscard]] bool HasRadianceSignature(std::span<const uint8> data) noexcept
{
    constexpr usize SIGNATURE_COUNT = 2;
    constexpr foundation::StaticArray<std::string_view, SIGNATURE_COUNT> SIGNATURES{"#?RADIANCE", "#?RGBE"};
    const std::string_view text(reinterpret_cast<const char*>(data.data()),
                                data.size() < RADIANCE_SIGNATURE_SCAN_BYTES ? data.size()
                                                                            : RADIANCE_SIGNATURE_SCAN_BYTES);
    for (const std::string_view signature : SIGNATURES)
    {
        if (text.substr(0, signature.size()) == signature)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasJpegSignature(std::span<const uint8> data) noexcept
{
    if (data.size() < JPEG_SIGNATURE_BYTES)
    {
        return false;
    }
    for (usize i = 0; i < JPEG_SIGNATURE_BYTES; ++i)
    {
        if (data[i] != JPEG_SIGNATURE[i])
        {
            return false;
        }
    }
    return true;
}

/// Decode a Radiance RGBE (.hdr) image into linear float RGB, top row first.
[[nodiscard]] ImageResult
DecodeRadiance(std::span<const uint8> data, DecodedLightingImage& outImage, std::string_view& outMessage) noexcept
{
    if (data.size() > MAX_STB_INPUT_BYTES)
    {
        outMessage = "Radiance HDR file is too large for the decoder";
        return ImageResult::Invalid;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &channels) == 0 ||
        width <= 0 || height <= 0 || static_cast<uint32>(width) > MAX_IMAGE_DIMENSION ||
        static_cast<uint32>(height) > MAX_IMAGE_DIMENSION ||
        static_cast<usize>(width) * static_cast<usize>(height) > MAX_DECODED_IMAGE_PIXELS)
    {
        outMessage = "Radiance HDR header is malformed or exceeds the image limits";
        return ImageResult::Invalid;
    }
    float* samplesOrNull = stbi_loadf_from_memory(data.data(),
                                                  static_cast<int>(data.size()),
                                                  &width,
                                                  &height,
                                                  &channels,
                                                  static_cast<int>(LIGHTING_IMAGE_CHANNEL_COUNT));
    if (samplesOrNull == nullptr)
    {
        if (HasStbRunOutOfMemory() == true)
        {
            return ImageResult::OutOfMemory;
        }
        outMessage = "Radiance HDR decoding failed";
        return ImageResult::Invalid;
    }
    float* samples = samplesOrNull;
    DecodedLightingImage candidate;
    candidate.Width = static_cast<uint32>(width);
    candidate.Height = static_cast<uint32>(height);
    const bool isAllocated =
        candidate.Rgb.TryResize(usize{candidate.Width} * candidate.Height * LIGHTING_IMAGE_CHANNEL_COUNT);
    for (usize i = 0; isAllocated == true && i < candidate.Rgb.GetSize(); ++i)
    {
        candidate.Rgb[i] = samples[i];
    }
    stbi_image_free(samples);
    if (isAllocated == false)
    {
        return ImageResult::OutOfMemory;
    }
    outImage = static_cast<DecodedLightingImage&&>(candidate);
    return ImageResult::Ok;
}

/// Decode a float HDR container (Radiance or OpenEXR) chosen by magic bytes.
[[nodiscard]] ImageResult
DecodeFloatImage(std::span<const uint8> data, DecodedLightingImage& outImage, std::string_view& outMessage) noexcept
{
    return HasExrMagic(data) == true ? DecodeExr(data, outImage, outMessage)
                                     : DecodeRadiance(data, outImage, outMessage);
}

/// Display encoding of float RGB for colour textures: clamp to [0, 1], then sRGB.
[[nodiscard]] ImageResult EncodeFloatImage(const DecodedLightingImage& source, DecodedImage& outImage) noexcept
{
    if (AllocateImage(source.Width, source.Height, outImage) == false)
    {
        return ImageResult::OutOfMemory;
    }
    outImage.WasHighDynamicRange = true;
    for (usize i = 0; i < usize{source.Width} * source.Height; ++i)
    {
        for (usize k = 0; k < LIGHTING_IMAGE_CHANNEL_COUNT; ++k)
        {
            float32 value = source.Rgb[i * LIGHTING_IMAGE_CHANNEL_COUNT + k];
            value = math::IsFinite(value) == true ? math::Clamp(value, 0.0F, 1.0F) : 0.0F;
            outImage.Texels[i * DECODED_IMAGE_CHANNEL_COUNT + k] = static_cast<uint8>(
                math::Floor(static_cast<float64>(math::EncodeSrgb(value)) * DECODED_IMAGE_CHANNEL_MAXIMUM + 0.5));
        }
        outImage.Texels[i * DECODED_IMAGE_CHANNEL_COUNT + ALPHA_CHANNEL_INDEX] = OPAQUE_ALPHA;
    }
    return ImageResult::Ok;
}

/// Halve float RGB with a 2x2 box filter (odd edges clamp); preserves mean radiance.
[[nodiscard]] bool HalveLightingImage(DecodedLightingImage& inoutImage) noexcept
{
    const uint32 width = inoutImage.Width > 1 ? inoutImage.Width / 2 : 1;
    const uint32 height = inoutImage.Height > 1 ? inoutImage.Height / 2 : 1;
    Array<float32> rgb;
    if (rgb.TryResize(usize{width} * height * LIGHTING_IMAGE_CHANNEL_COUNT) == false)
    {
        return false;
    }
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            for (usize k = 0; k < LIGHTING_IMAGE_CHANNEL_COUNT; ++k)
            {
                float64 sum = 0.0;
                for (uint32 dy = 0; dy < 2; ++dy)
                {
                    for (uint32 dx = 0; dx < 2; ++dx)
                    {
                        const uint32 sx = math::Min(x * 2 + dx, inoutImage.Width - 1);
                        const uint32 sy = math::Min(y * 2 + dy, inoutImage.Height - 1);
                        sum += static_cast<float64>(
                            inoutImage.Rgb[(usize{sy} * inoutImage.Width + sx) * LIGHTING_IMAGE_CHANNEL_COUNT + k]);
                    }
                }
                rgb[(usize{y} * width + x) * LIGHTING_IMAGE_CHANNEL_COUNT + k] =
                    static_cast<float32>(sum * BOX_FILTER_TAP_WEIGHT);
            }
        }
    }
    inoutImage.Rgb.Swap(rgb);
    inoutImage.Width = width;
    inoutImage.Height = height;
    return true;
}

[[nodiscard]] ImageResult
DecodePfm(std::span<const uint8> data, DecodedImage& outImage, std::string_view& outMessage) noexcept
{
    // Header: "PF" or "Pf", width height, scale (negative = little-endian), each
    // followed by whitespace; rows run bottom to top.
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size() < 256 ? data.size() : 256);
    std::string_view words[PFM_HEADER_WORD_COUNT];
    usize at = 0;
    for (std::string_view& word : words)
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
        {
            ++at;
        }
        const usize start = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\n' && text[at] != '\r' && text[at] != '\t')
        {
            ++at;
        }
        word = text.substr(start, at - start);
    }
    ++at; // The single whitespace byte before the raster.
    float64 width = 0.0;
    float64 height = 0.0;
    float64 scale = 0.0;
    const bool isColour = words[PFM_IDENTIFIER_WORD] == "PF";
    if ((isColour == false && words[PFM_IDENTIFIER_WORD] != "Pf") ||
        ParseNumber(words[PFM_WIDTH_WORD], width) == false || ParseNumber(words[PFM_HEIGHT_WORD], height) == false ||
        ParseNumber(words[PFM_SCALE_WORD], scale) == false || width < 1.0 || height < 1.0 ||
        width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION || scale == 0.0)
    {
        outMessage = "malformed PFM header";
        return ImageResult::Invalid;
    }
    const uint32 columns = static_cast<uint32>(width);
    const uint32 rows = static_cast<uint32>(height);
    const usize channels = isColour == true ? 3 : 1;
    if (at > data.size() || (data.size() - at) / 4 / channels / columns < rows)
    {
        outMessage = "PFM raster is truncated";
        return ImageResult::Invalid;
    }
    if (AllocateImage(columns, rows, outImage) == false)
    {
        return ImageResult::OutOfMemory;
    }
    outImage.WasHighDynamicRange = true;
    const bool isLittleEndian = scale < 0.0;
    for (uint32 y = 0; y < rows; ++y)
    {
        for (uint32 x = 0; x < columns; ++x)
        {
            uint8* target = outImage.Texels.GetData() + ((usize{rows} - 1 - y) * columns + x) * 4;
            for (usize k = 0; k < 3; ++k)
            {
                const usize channel = channels == 3 ? k : 0;
                const usize offset = at + ((usize{y} * columns + x) * channels + channel) * 4;
                uint32 bits = 0;
                for (usize b = 0; b < 4; ++b)
                {
                    const usize shift = isLittleEndian == true ? b : 3 - b;
                    bits |= static_cast<uint32>(data[offset + b]) << (8 * shift);
                }
                float32 value = std::bit_cast<float32>(bits);
                value = math::IsFinite(value) == true ? math::Clamp(value, 0.0F, 1.0F) : 0.0F;
                target[k] =
                    static_cast<uint8>(math::Floor(static_cast<float64>(math::EncodeSrgb(value)) * 255.0 + 0.5));
            }
            target[3] = 255;
        }
    }
    return ImageResult::Ok;
}
} // namespace

ImageResult DecodeImage(std::string_view fileName,
                        std::span<const uint8> data,
                        DecodedImage& outImage,
                        std::string_view& outMessage) noexcept
{
    outImage.Width = 0;
    outImage.Height = 0;
    outImage.Texels.Clear();
    outImage.WasHighDynamicRange = false;
    static constexpr uint8 PNG_MAGIC[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    bool isPng = data.size() >= 8;
    for (usize i = 0; isPng == true && i < 8; ++i)
    {
        isPng = data[i] == PNG_MAGIC[i];
    }
    if (isPng == true)
    {
        return DecodeWithStb(data, "PNG decoding failed", outImage, outMessage);
    }
    if (HasJpegSignature(data) == true)
    {
        return DecodeWithStb(data, "JPEG decoding failed", outImage, outMessage);
    }
    if (HasExrMagic(data) == true || HasRadianceSignature(data) == true)
    {
        DecodedLightingImage linear;
        const ImageResult decoded = DecodeFloatImage(data, linear, outMessage);
        return decoded == ImageResult::Ok ? EncodeFloatImage(linear, outImage) : decoded;
    }
    if (HasExtension(fileName, ".tga") == true)
    {
        return DecodeTga(data, outImage, outMessage);
    }
    if (HasExtension(fileName, ".pfm") == true)
    {
        return DecodePfm(data, outImage, outMessage);
    }
    outMessage = "no decoder for this image format in this build";
    return ImageResult::Unsupported;
}
ImageResult DecodeLightingImage(std::string_view fileName,
                                foundation::BorrowedView<const uint8> data,
                                DecodedLightingImage& outImage,
                                std::string_view& outMessage,
                                bool shouldFactorRadiance) noexcept
{
    if (data.IsValid() == false || data.IsEmpty() == true)
    {
        outMessage = "invalid lighting image bytes";
        return ImageResult::Invalid;
    }
    const float32 maximumRadiance = content::scene_format::MAX_EMISSION;
    const float64 maximumInput =
        static_cast<float64>(shouldFactorRadiance == true ? maximumRadiance * maximumRadiance : maximumRadiance);
    DecodedLightingImage candidate;
    const std::span<const uint8> bytes(data.GetDataOrNull(), data.GetSize());
    if (HasExrMagic(bytes) == true || HasRadianceSignature(bytes) == true)
    {
        // Radiance and OpenEXR maps keep their linear energy. Larger maps are box-halved
        // (mean radiance preserved) until they fit the bounded environment profile.
        const ImageResult decoded = DecodeFloatImage(bytes, candidate, outMessage);
        if (decoded != ImageResult::Ok)
        {
            return decoded;
        }
        while (candidate.Width > content::scene_format::MAX_ENVIRONMENT_DIMENSION ||
               candidate.Height > content::scene_format::MAX_ENVIRONMENT_DIMENSION ||
               usize{candidate.Width} * candidate.Height >
                   content::scene_format::MAX_BUNDLE_BYTES / (LIGHTING_IMAGE_CHANNEL_COUNT * sizeof(float32)))
        {
            if (HalveLightingImage(candidate) == false)
            {
                return ImageResult::OutOfMemory;
            }
        }
        for (const float32 sample : candidate.Rgb)
        {
            const float64 value = static_cast<float64>(sample);
            if (math::IsFinite(value) == false || value < 0 || value > maximumInput)
            {
                outMessage = "HDR lighting contains unsupported negative/nonfinite or over-range energy";
                return ImageResult::Invalid;
            }
        }
    }
    else if (HasExtension(fileName, ".pfm") == false)
    {
        DecodedImage encoded;
        const ImageResult status = DecodeImage(fileName, {data.GetDataOrNull(), data.GetSize()}, encoded, outMessage);
        if (status != ImageResult::Ok)
        {
            return status;
        }
        candidate.Width = encoded.Width;
        candidate.Height = encoded.Height;
        if (candidate.Width > content::scene_format::MAX_ENVIRONMENT_DIMENSION ||
            candidate.Height > content::scene_format::MAX_ENVIRONMENT_DIMENSION ||
            usize{candidate.Width} * candidate.Height > content::scene_format::MAX_BUNDLE_BYTES / (3 * sizeof(float32)))
        {
            outMessage = "lighting image exceeds the bounded scene profile";
            return ImageResult::Invalid;
        }
        if (candidate.Rgb.TryResize(usize{candidate.Width} * candidate.Height * 3) == false)
        {
            return ImageResult::OutOfMemory;
        }
        for (usize i = 0; i < candidate.Rgb.GetSize(); ++i)
        {
            candidate.Rgb[i] = math::DecodeSrgb(static_cast<float32>(encoded.Texels[i / 3 * 4 + i % 3]) / 255.0F);
        }
    }
    else
    {
        // Debevec's PFM layout: signed scale selects endian order, magnitude
        // scales linear samples; rows run bottom to top. Keep HDR energy intact.
        // https://www.pauldebevec.com/Research/HDR/PFM/
        const std::string_view header(reinterpret_cast<const char*>(data.GetDataOrNull()),
                                      data.GetSize() < 256 ? data.GetSize() : 256);
        constexpr usize HEADER_WORDS = 4;
        foundation::StaticArray<std::string_view, HEADER_WORDS> words{};
        usize at = 0;
        for (std::string_view& word : words)
        {
            while (at < header.size() &&
                   (header[at] == ' ' || header[at] == '\n' || header[at] == '\r' || header[at] == '\t'))
            {
                ++at;
            }
            const usize begin = at;
            while (at < header.size() && header[at] != ' ' && header[at] != '\n' && header[at] != '\r' &&
                   header[at] != '\t')
            {
                ++at;
            }
            word = header.substr(begin, at - begin);
        }
        if (at >= header.size())
        {
            outMessage = "truncated PFM lighting header";
            return ImageResult::Invalid;
        }
        if (header[at] == '\r' && at + 1 < header.size() && header[at + 1] == '\n')
        {
            ++at;
        }
        ++at;
        float64 width = 0;
        float64 height = 0;
        float64 scale = 0;
        const bool isColour = words[PFM_IDENTIFIER_WORD] == "PF";
        if ((isColour == false && words[PFM_IDENTIFIER_WORD] != "Pf") ||
            ParseNumber(words[PFM_WIDTH_WORD], width) == false ||
            ParseNumber(words[PFM_HEIGHT_WORD], height) == false ||
            ParseNumber(words[PFM_SCALE_WORD], scale) == false || width < 1 || height < 1 ||
            width > content::scene_format::MAX_ENVIRONMENT_DIMENSION ||
            height > content::scene_format::MAX_ENVIRONMENT_DIMENSION || math::Floor(width) != width ||
            math::Floor(height) != height || scale == 0)
        {
            outMessage = "invalid PFM lighting dimensions or scale";
            return ImageResult::Invalid;
        }
        candidate.Width = static_cast<uint32>(width);
        candidate.Height = static_cast<uint32>(height);
        const usize channels = isColour == true ? 3 : 1;
        const usize count = usize{candidate.Width} * candidate.Height;
        if (count > content::scene_format::MAX_BUNDLE_BYTES / (3 * sizeof(float32)))
        {
            outMessage = "lighting image exceeds the bounded scene byte budget";
            return ImageResult::Invalid;
        }
        if (at > data.GetSize() || (data.GetSize() - at) / sizeof(float32) / channels < count)
        {
            outMessage = "truncated PFM lighting raster";
            return ImageResult::Invalid;
        }
        if (candidate.Rgb.TryResize(count * 3) == false)
        {
            return ImageResult::OutOfMemory;
        }
        for (uint32 y = 0; y < candidate.Height; ++y)
        {
            for (uint32 x = 0; x < candidate.Width; ++x)
            {
                for (usize channel = 0; channel < 3; ++channel)
                {
                    const usize source =
                        at +
                        ((usize{y} * candidate.Width + x) * channels + (channels == 3 ? channel : 0)) * sizeof(float32);
                    uint32 bits = 0;
                    for (usize byte = 0; byte < sizeof(float32); ++byte)
                    {
                        bits |= static_cast<uint32>(data[source + byte]) << (8 * (scale < 0 ? byte : 3 - byte));
                    }
                    float32 sample = 0;
                    std::memcpy(&sample, &bits, sizeof(sample));
                    const float64 value = static_cast<float64>(sample) * math::Abs(scale);
                    if (math::IsFinite(value) == false || value < 0 || value > maximumInput)
                    {
                        outMessage = "PFM lighting contains unsupported negative/nonfinite or over-range energy";
                        return ImageResult::Invalid;
                    }
                    candidate.Rgb[((usize{candidate.Height} - 1 - y) * candidate.Width + x) * 3 + channel] =
                        static_cast<float32>(value);
                }
            }
        }
    }
    if (shouldFactorRadiance == true)
    {
        float32 peak = 0;
        for (const float32 value : candidate.Rgb)
        {
            peak = math::Max(peak, value);
        }
        // Power-of-two factoring avoids extra rounding for normal binary32 samples without clipping
        // the sun. Both factors must fit the existing LUSCN lighting representation.
        while (peak / candidate.RadianceScale > maximumRadiance)
        {
            candidate.RadianceScale *= 2;
        }
        if (candidate.RadianceScale > maximumRadiance)
        {
            outMessage = "HDR radiance multiplier exceeds the lighting profile";
            return ImageResult::Invalid;
        }
        for (float32& value : candidate.Rgb)
        {
            value /= candidate.RadianceScale;
        }
    }
    outImage = static_cast<DecodedLightingImage&&>(candidate);
    outMessage = {};
    return ImageResult::Ok;
}
} // namespace ludus::content::scene_cook::importers
