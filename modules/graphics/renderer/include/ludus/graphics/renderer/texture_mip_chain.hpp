#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// Most levels a chain built here holds; the RHI stores at most sixteen levels per texture.
inline constexpr foundation::uint32 MAX_TEXTURE_MIP_LEVELS = 16;
/// A complete RGBA8 mip chain laid out for one rhi::TextureUpload: Bytes holds every level tightly
/// packed in ascending order and Mips describes each level's offset and row pitch.
struct Rgba8MipChain final
{
    /// Tightly packed levels, level zero first; four-byte aligned because every RGBA8 row is.
    foundation::Array<foundation::uint8> Bytes;
    /// One descriptor per level, in ascending order, for rhi::TextureUpload::Mips.
    foundation::Array<rhi::TextureMipUpload> Mips;
    /// Number of levels in Bytes and Mips; zero before a successful build.
    foundation::uint32 LevelCount = 0;
};
/// Count the complete levels of a width by height chain down to one texel: floor(log2(max(width, height))) + 1,
/// bounded by MAX_TEXTURE_MIP_LEVELS. Zero extents count as one level.
[[nodiscard]] foundation::uint32 CountRgba8MipLevels(foundation::uint32 width, foundation::uint32 height) noexcept;
/// Build the first levelCount levels of source's mip chain with a 2x2 box filter. Level zero copies
/// the source rows without their padding. Rgba8Srgb colour channels are averaged in linear light and
/// re-encoded; Rgba8Unorm channels and every alpha channel average the stored codes, so linear data such
/// as packed surface parameters keeps its mean. A level with an odd parent extent clamps the second
/// parent texel to the edge.
/// @param source Complete rows of level zero in Rgba8Unorm or Rgba8Srgb with RowPitch >= Width * 4.
/// @param levelCount Levels to build, 1..CountRgba8MipLevels(source.Width, source.Height).
/// @return Ready with every level in outChain; InvalidDescription for a malformed source, format or count,
///         or OutOfMemory when the chain cannot be allocated. Failure preserves outChain.
[[nodiscard]] rhi::RasterStatus BuildRgba8MipChain(const rhi::TextureCpuContent& source,
                                                   foundation::uint32 levelCount,
                                                   Rgba8MipChain& outChain) noexcept;
} // namespace ludus::graphics::renderer
