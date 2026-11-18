#include <ludus/graphics/renderer/texture_mip_chain.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
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
constexpr usize MAX_AXIS_TAPS = 3;
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
/// Texels of one chain level, tightly packed in rows.
struct LevelView final
{
    BorrowedView<const uint8> Texels;
    uint32 Width = 0;
    uint32 Height = 0;
};
/// Consecutive parent texels and their relative overlap widths as integers.
struct AxisFootprint final
{
    uint32 FirstTexel;
    usize TapCount;
    uint32 TotalWeight;
    StaticArray<uint32, MAX_AXIS_TAPS> Weights;
};

// Thanks to Mehdi NS, “Notes on Anti-Aliasing: Texture filtering (part 3),” for prompting the minification review.
// https://mehdins.wordpress.com/2017/08/29/notes-on-anti-aliasing-texture-filtering-part-3/
AxisFootprint ComputeAxisFootprint(uint32 parentExtent, uint32 childTexel) noexcept
{
    if (parentExtent == 1)
    {
        return {0, 1, 1, {{1, 0, 0}}};
    }
    const uint32 firstTexel = childTexel * 2;
    if (parentExtent % 2 == 0)
    {
        return {firstTexel, 2, 2, {{1, 1, 0}}};
    }
    // Match the footprint geometry in cmake/shaders/cook_texture.py so odd edges contribute.
    // With n = floor(parentExtent / 2), the box spans [2i + i/n, 2i + 2 + (i+1)/n].
    const uint32 childExtent = parentExtent / 2;
    return {firstTexel, MAX_AXIS_TAPS, parentExtent, {{childExtent - childTexel, childExtent, childTexel + 1}}};
}

void FilterTexel(const LevelView& parent,
                 const AxisFootprint& horizontal,
                 const AxisFootprint& vertical,
                 bool isSrgb,
                 uint8* outTexel) noexcept
{
    const SrgbDecodeTable& decodeTable = GetSrgbDecodeTable();
    const float32 area = static_cast<float32>(horizontal.TotalWeight) * static_cast<float32>(vertical.TotalWeight);
    StaticArray<float32, CHANNEL_COUNT> sums{};
    for (usize row = 0; row < vertical.TapCount; ++row)
    {
        const usize rowOffset = (vertical.FirstTexel + row) * parent.Width;
        const float32 rowWeight = static_cast<float32>(vertical.Weights[row]);
        for (usize column = 0; column < horizontal.TapCount; ++column)
        {
            const usize offset = (rowOffset + horizontal.FirstTexel + column) * CHANNEL_COUNT;
            const float32 weight = rowWeight * static_cast<float32>(horizontal.Weights[column]);
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                const uint8 code = parent.Texels[offset + channel];
                const bool isColor = isSrgb == true && channel < COLOR_CHANNEL_COUNT;
                sums[channel] +=
                    weight * (isColor == true ? decodeTable.Linear[code] : static_cast<float32>(code) / CODE_MAXIMUM);
            }
        }
    }
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const bool isColor = isSrgb == true && channel < COLOR_CHANNEL_COUNT;
        const float32 mean = sums[channel] / area;
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
        const LevelView parent{
            {chain.Bytes.GetData() + parentOffset, usize{parentWidth} * CHANNEL_COUNT * parentHeight},
            parentWidth,
            parentHeight};
        const uint32 width = ComputeLevelExtent(source.Width, level);
        const uint32 height = ComputeLevelExtent(source.Height, level);
        uint8* texels = chain.Bytes.GetData() + offset;
        for (uint32 y = 0; y < height; ++y)
        {
            const AxisFootprint vertical = ComputeAxisFootprint(parent.Height, y);
            for (uint32 x = 0; x < width; ++x)
            {
                const AxisFootprint horizontal = ComputeAxisFootprint(parent.Width, x);
                FilterTexel(parent, horizontal, vertical, isSrgb, texels + (usize{y} * width + x) * CHANNEL_COUNT);
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
