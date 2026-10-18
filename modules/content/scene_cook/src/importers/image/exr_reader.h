#pragma once

// Bounded OpenEXR subset reader for HDR environment maps and textures.
//
// Accepted profile: single-part scanline or tiled images (level 0 of mipmapped
// and ripmapped files), HALF/FLOAT/UINT channels with unit sampling, and NONE,
// RLE, ZIPS or ZIP compression. R/G/B channels (or a single Y channel) are
// returned as linear float RGB; alpha and other channels are skipped. PIZ,
// PXR24, B44(A), DWAA/DWAB, deep data and multi-part files are reported as
// Unsupported rather than approximated.

#include "image_reader.h"

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// Largest decoded HDR image in pixels (8192 x 8192), bounding float output memory.
inline constexpr usize MAX_HDR_IMAGE_PIXELS = usize{8192} * 8192;

/// Decode an OpenEXR image into linear float RGB, top row first.
/// @param data Complete file bytes.
/// @param outImage Replaced only on Ok.
/// @param outMessage Static description of a failure.
/// @return Ok; Unsupported for valid files outside the profile; Invalid for malformed
/// or over-limit files; OutOfMemory.
[[nodiscard]] ImageResult
DecodeExr(std::span<const uint8> data, DecodedLightingImage& outImage, std::string_view& outMessage) noexcept;

/// True when data starts with the OpenEXR magic number.
[[nodiscard]] bool HasExrMagic(std::span<const uint8> data) noexcept;
} // namespace ludus::content::scene_cook::importers
