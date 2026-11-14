#pragma once
// Thanks to Khronos, Vulkan Specification, "Copying Data Between Buffers and Images",
// https://docs.vulkan.org/spec/latest/chapters/copies.html#copies-buffers-images,
// and W3C GPU for the Web Working Group, WebGPU, "Copying texture data",
// https://www.w3.org/TR/webgpu/#copying-texture-data, for block-row and mip bounds.
// R9 keeps the encoded format and caller padding; it performs no conversion.
// Thanks to Microsoft, "Block Compression (Direct3D 10)", BC5:
// https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
// BC5_UNORM stores two independent normalized channels in each 16-byte block.
// Native sampling supplies (R,G,0,1); interpreting/reconstructing normals is caller policy.
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi::internal
{
inline bool TextureBinding(RasterBindingKind kind) noexcept
{
    return kind == RasterBindingKind::TextureDepth2D || kind == RasterBindingKind::TextureStencil2D ||
           kind == RasterBindingKind::Texture2D || kind == RasterBindingKind::Texture2DArray ||
           kind == RasterBindingKind::TextureCube || kind == RasterBindingKind::Texture3D;
}
// Sampled kinds above; write-only storage views are a separate R12b kind.
inline bool StorageTextureBinding(RasterBindingKind kind) noexcept
{
    return kind == RasterBindingKind::StorageTexture2D;
}
// Any binding whose snapshot slot names a texture view, sampled or storage.
inline bool ViewBinding(RasterBindingKind kind) noexcept
{
    return TextureBinding(kind) == true || StorageTextureBinding(kind) == true;
}
inline RasterBindingKind TextureBindingType(TextureDimension dimension) noexcept
{
    return dimension == TextureDimension::D2Array ? RasterBindingKind::Texture2DArray
           : dimension == TextureDimension::Cube  ? RasterBindingKind::TextureCube
           : dimension == TextureDimension::D3    ? RasterBindingKind::Texture3D
                                                  : RasterBindingKind::Texture2D;
}
inline bool CompressedTexture(RasterFormat format) noexcept
{
    return format == RasterFormat::Bc1RgbaUnorm || format == RasterFormat::Bc3RgbaUnorm ||
           format == RasterFormat::Bc5RgUnorm;
}
inline foundation::uint32 TextureBlockWidth(RasterFormat format) noexcept
{
    return CompressedTexture(format) ? 4U : 1U;
}
inline foundation::usize TextureBlockBytes(RasterFormat format) noexcept
{
    return format == RasterFormat::Bc1RgbaUnorm                                         ? 8U
           : format == RasterFormat::Bc3RgbaUnorm || format == RasterFormat::Bc5RgUnorm ? 16U
           : format == RasterFormat::Rgba16Float                                        ? 8U
           : format == RasterFormat::Rg8Unorm                                           ? 2U
           : format == RasterFormat::R8Unorm                                            ? 1U
                                                                                        : 4U;
}
inline foundation::uint32 TextureMipExtent(foundation::uint32 extent, foundation::uint32 mip) noexcept
{
    return (extent >> mip) != 0 ? extent >> mip : 1U;
}
inline foundation::uint32 TextureSlices(const TextureDescription& info, foundation::uint32 mip) noexcept
{
    return info.Dimension == TextureDimension::D3 ? TextureMipExtent(info.DepthOrLayers, mip) : info.DepthOrLayers;
}
inline foundation::uint32 TextureBlockRows(RasterFormat format, foundation::uint32 height) noexcept
{
    const foundation::uint32 block = TextureBlockWidth(format);
    return height / block + (height % block != 0 ? 1U : 0U);
}
} // namespace ludus::graphics::rhi::internal
