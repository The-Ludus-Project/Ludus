#pragma once

#include "image_reader.h"

namespace ludus::content::scene_cook::importers
{
/// Meaning of the stored channels when reducing an imported texture.
enum class ImageReductionProfile : uint8
{
    SrgbColor,     ///< Decode RGB before filtering; alpha remains linear coverage.
    LinearData,    ///< Independently filter linear scalar channels.
    TangentNormal, ///< Reconstruct and average RG normals; BA remain linear scalar multipliers.
};

/// Reduce mip zero by a two-by-two box, clamping one-texel dimensions.
/// The destination extent is floor(source/2), with a minimum of one texel.
/// @return False for invalid storage or allocation failure, preserving inoutImage.
[[nodiscard]] bool TryHalveImage(DecodedImage& inoutImage, ImageReductionProfile profile) noexcept;
} // namespace ludus::content::scene_cook::importers
