#include <ludus/graphics/renderer/texture_mip_chain.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cstring>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
constexpr usize CHANNEL_COUNT = 4;
constexpr usize COLOR_CHANNEL_COUNT = 3;
constexpr usize PARENT_TEXELS_PER_TEXEL = 4;
constexpr usize CODE_COUNT = 256;
constexpr float32 CODE_MAXIMUM = 255;
/// Linear light of every sRGB byte code, so filtering never evaluates the transfer per texel.
struct SrgbDecodeTable final
{
    StaticArray<float32, CODE_COUNT> Linear{};
    SrgbDecodeTable() noexcept
    {
        for (usize code = 0; code < CODE_COUNT; ++code)
        {
            Linear[code] = math::DecodeSrgb(static_cast<float32>(code) / CODE_MAXIMUM);
        }
    }
};
const SrgbDecodeTable& GetSrgbDecodeTable() noexcept
{
    static const SrgbDecodeTable TABLE;
    return TABLE;
}
uint8 EncodeCode(float32 unit) noexcept
{
    const float64 scaled = static_cast<float64>(math::Clamp(unit, 0.0F, 1.0F)) * static_cast<float64>(CODE_MAXIMUM);
    return static_cast<uint8>(math::Floor(scaled + 0.5));
}
uint32 ComputeLevelExtent(uint32 extent, uint32 level) noexcept
{
    const uint32 shifted = level < 32 ? extent >> level : 0;
    return shifted == 0 ? 1 : shifted;
}
/// Average the 2x2 parent block of one level texel into its four channels.
void FilterTexel(const uint8* parent,
                 uint32 parentWidth,
                 uint32 parentHeight,
                 uint32 x,
                 uint32 y,
                 bool isSrgb,
                 uint8* outTexel) noexcept
{
    const SrgbDecodeTable& decodeTable = GetSrgbDecodeTable();
    const uint32 x0 = x * 2 < parentWidth ? x * 2 : parentWidth - 1;
    const uint32 y0 = y * 2 < parentHeight ? y * 2 : parentHeight - 1;
    const uint32 x1 = x0 + 1 < parentWidth ? x0 + 1 : x0;
    const uint32 y1 = y0 + 1 < parentHeight ? y0 + 1 : y0;
    const StaticArray<usize, PARENT_TEXELS_PER_TEXEL> offsets{{(usize{y0} * parentWidth + x0) * CHANNEL_COUNT,
                                                               (usize{y0} * parentWidth + x1) * CHANNEL_COUNT,
                                                               (usize{y1} * parentWidth + x0) * CHANNEL_COUNT,
                                                               (usize{y1} * parentWidth + x1) * CHANNEL_COUNT}};
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const bool isColor = isSrgb == true && channel < COLOR_CHANNEL_COUNT;
        float32 sum = 0;
        for (const usize offset : offsets)
        {
            const uint8 code = parent[offset + channel];
            sum += isColor == true ? decodeTable.Linear[code] : static_cast<float32>(code) / CODE_MAXIMUM;
        }
        const float32 mean = sum / static_cast<float32>(PARENT_TEXELS_PER_TEXEL);
        outTexel[channel] = EncodeCode(isColor == true ? math::EncodeSrgb(mean) : mean);
    }
}
} // namespace

uint32 CountRgba8MipLevels(uint32 width, uint32 height) noexcept
{
    const uint32 extent = width > height ? width : height;
    uint32 levels = 1;
    while (levels < MAX_TEXTURE_MIP_LEVELS && (extent >> levels) != 0)
    {
        ++levels;
    }
    return levels;
}

rhi::RasterStatus
BuildRgba8MipChain(const rhi::TextureCpuContent& source, uint32 levelCount, Rgba8MipChain& outChain) noexcept
{
    const bool isSrgb = source.Format == rhi::RasterFormat::Rgba8Srgb;
    usize rowBytes = 0;
    usize levelZeroBytes = 0;
    if (source.Bytes == nullptr || source.Width == 0 || source.Height == 0 ||
        (isSrgb == false && source.Format != rhi::RasterFormat::Rgba8Unorm) || levelCount == 0 ||
        levelCount > CountRgba8MipLevels(source.Width, source.Height) ||
        TryMultiply(usize{source.Width}, CHANNEL_COUNT, rowBytes) == false || source.RowPitch < rowBytes ||
        TryMultiply(source.RowPitch, usize{source.Height}, levelZeroBytes) == false || source.Size < levelZeroBytes)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    usize totalBytes = 0;
    for (uint32 level = 0; level < levelCount; ++level)
    {
        usize levelBytes = 0;
        if (TryMultiply(usize{ComputeLevelExtent(source.Width, level)} * CHANNEL_COUNT,
                        usize{ComputeLevelExtent(source.Height, level)},
                        levelBytes) == false ||
            TryAdd(totalBytes, levelBytes, totalBytes) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
    }
    Rgba8MipChain chain;
    if (chain.Bytes.TryResize(totalBytes) == false || chain.Mips.TryResize(levelCount) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize row = 0; row < source.Height; ++row)
    {
        std::memcpy(chain.Bytes.GetData() + row * rowBytes, source.Bytes + row * source.RowPitch, rowBytes);
    }
    chain.Mips[0] = {0, rowBytes};
    usize parentOffset = 0;
    usize offset = rowBytes * source.Height;
    for (uint32 level = 1; level < levelCount; ++level)
    {
        const uint32 parentWidth = ComputeLevelExtent(source.Width, level - 1);
        const uint32 parentHeight = ComputeLevelExtent(source.Height, level - 1);
        const uint32 width = ComputeLevelExtent(source.Width, level);
        const uint32 height = ComputeLevelExtent(source.Height, level);
        const uint8* parent = chain.Bytes.GetData() + parentOffset;
        uint8* texels = chain.Bytes.GetData() + offset;
        for (uint32 y = 0; y < height; ++y)
        {
            for (uint32 x = 0; x < width; ++x)
            {
                FilterTexel(parent,
                            parentWidth,
                            parentHeight,
                            x,
                            y,
                            isSrgb,
                            texels + (usize{y} * width + x) * CHANNEL_COUNT);
            }
        }
        chain.Mips[level] = {offset, usize{width} * CHANNEL_COUNT};
        parentOffset = offset;
        offset += usize{width} * CHANNEL_COUNT * height;
    }
    chain.LevelCount = levelCount;
    outChain = Move(chain);
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer
