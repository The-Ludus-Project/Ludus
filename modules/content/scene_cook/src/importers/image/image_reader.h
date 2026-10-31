#pragma once

// Texture image decoding for the best-effort importers: TGA (including RLE and
// colour-mapped), PNG and baseline/progressive JPEG (vendored stb_image),
// Radiance RGBE (.hdr), an OpenEXR subset (exr_reader.h) and PFM. Colour output
// is RGBA8, rows top to bottom, with colour channels kept in the file's encoding
// (HDR sources are clamped and sRGB-encoded). EXIF orientation and embedded ICC
// profiles are ignored. Lighting output keeps linear float RGB.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers
{
using foundation::Array;
using foundation::core::float32;
using foundation::core::float64;
using foundation::core::uint32;
using foundation::core::uint8;
using foundation::core::usize;

/// Largest decoded image side.
inline constexpr uint32 MAX_IMAGE_DIMENSION = 16384;
/// Largest decoded raster in pixels (8192 x 8192), checked from the header before any
/// decoding so a small file cannot declare a huge image (JPEG, PNG, Radiance, OpenEXR).
inline constexpr usize MAX_DECODED_IMAGE_PIXELS = usize{8192} * 8192;

/// Channels per pixel in DecodedImage::Texels (RGBA8).
inline constexpr usize DECODED_IMAGE_CHANNEL_COUNT = 4;
/// Full-scale value of an 8-bit DecodedImage channel.
inline constexpr float64 DECODED_IMAGE_CHANNEL_MAXIMUM = 255.0;
/// Channels per pixel in DecodedLightingImage::Rgb (linear float RGB).
inline constexpr usize LIGHTING_IMAGE_CHANNEL_COUNT = 3;

/// Decoded RGBA8 image.
struct DecodedImage final
{
    uint32 Width = 0;
    uint32 Height = 0;
    /// Width * Height * DECODED_IMAGE_CHANNEL_COUNT bytes, top row first.
    Array<uint8> Texels;
    /// True when the source stored linear (HDR) values that were tone-mapped and sRGB-encoded.
    bool WasHighDynamicRange = false;
};

/// Lighting texels retain linear HDR values; no display encoding or tone mapping.
struct DecodedLightingImage final
{
    /// Multiply RGB samples by this value to reconstruct factored HDR radiance.
    float32 RadianceScale = 1;
    uint32 Width = 0;
    uint32 Height = 0;
    /// Width * Height * LIGHTING_IMAGE_CHANNEL_COUNT samples, top row first.
    Array<float32> Rgb;
};

/// Outcome of DecodeImage.
enum class ImageResult : uint8
{
    Ok,
    /// The container (for example TIFF) or a valid feature of it (for example EXR
    /// PIZ compression) has no decoder in this build.
    Unsupported,
    Invalid,
    OutOfMemory,
};

/// Decode data, choosing the format from the file name extension (case-insensitive)
/// and the magic bytes.
[[nodiscard]] ImageResult DecodeImage(std::string_view fileName,
                                      std::span<const uint8> data,
                                      DecodedImage& outImage,
                                      std::string_view& outMessage) noexcept;
/// PFM lighting preserves finite nonnegative float RGB and file scale, north row first.
/// Radiance and OpenEXR lighting keep linear float RGB and are box-halved until they fit
/// MAX_ENVIRONMENT_DIMENSION and the bundle byte budget. When shouldFactorRadiance
/// is true, high peaks use RadianceScale and bounded texels instead of clipping;
/// their product preserves linear radiance within binary32 precision. Both factors
/// must fit the existing scene profile. False retains the frozen rejection policy.
/// PNG/TGA/JPEG lighting is
/// decoded from sRGB. Failure preserves outImage.
[[nodiscard]] ImageResult DecodeLightingImage(std::string_view fileName,
                                              foundation::BorrowedView<const uint8> data,
                                              DecodedLightingImage& outImage,
                                              std::string_view& outMessage,
                                              bool shouldFactorRadiance = false) noexcept;
} // namespace ludus::content::scene_cook::importers
