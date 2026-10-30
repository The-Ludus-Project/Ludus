#include "image_filter.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::content::scene_cook::importers
{
namespace math = foundation::math;
namespace
{
constexpr usize FILTER_SAMPLE_COUNT = 4;
constexpr usize COLOR_CHANNEL_COUNT = 3;
constexpr usize CHANNEL_VALUE_COUNT = static_cast<usize>(DECODED_IMAGE_CHANNEL_MAXIMUM) + 1;
constexpr float32 CHANNEL_MAXIMUM = static_cast<float32>(DECODED_IMAGE_CHANNEL_MAXIMUM);
constexpr float32 FILTER_WEIGHT = 1.0F / static_cast<float32>(FILTER_SAMPLE_COUNT);

uint8 EncodeUnitChannel(float32 value) noexcept
{
    return static_cast<uint8>(
        math::Floor(static_cast<float64>(math::Clamp(value, 0.0F, 1.0F) * CHANNEL_MAXIMUM + 0.5F)));
}
} // namespace

bool TryHalveImage(DecodedImage& inoutImage, ImageReductionProfile profile) noexcept
{
    usize sourceBytes = 0;
    if (inoutImage.Width == 0 || inoutImage.Height == 0 ||
        foundation::TryMultiply(usize{inoutImage.Width}, usize{inoutImage.Height}, sourceBytes) == false ||
        foundation::TryMultiply(sourceBytes, DECODED_IMAGE_CHANNEL_COUNT, sourceBytes) == false ||
        sourceBytes != inoutImage.Texels.GetSize())
    {
        return false;
    }
    const uint32 width = inoutImage.Width > 1 ? inoutImage.Width / 2 : 1;
    const uint32 height = inoutImage.Height > 1 ? inoutImage.Height / 2 : 1;
    Array<uint8> texels;
    if (texels.TryResize(usize{width} * height * DECODED_IMAGE_CHANNEL_COUNT) == false)
    {
        return false;
    }
    foundation::StaticArray<float32, CHANNEL_VALUE_COUNT> linearChannels{};
    if (profile == ImageReductionProfile::SrgbColor)
    {
        for (usize value = 0; value < CHANNEL_VALUE_COUNT; ++value)
        {
            linearChannels[value] = math::DecodeSrgb(static_cast<float32>(value) / CHANNEL_MAXIMUM);
        }
    }
    // Thanks to Khronos, glTF 2.0 Specification, sections 3.9.2 and 3.9.3:
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
    // Color arithmetic uses linear RGB, while tangent-normal components are linear
    // directions. The packed surface profile reconstructs positive Z before reduction.
    for (uint32 y = 0; y < height; ++y)
    {
        const uint8* top = inoutImage.Texels.GetData() + usize{y} * 2 * inoutImage.Width * DECODED_IMAGE_CHANNEL_COUNT;
        const uint8* bottom = inoutImage.Texels.GetData() + usize{math::Min(y * 2 + 1, inoutImage.Height - 1)} *
                                                                inoutImage.Width * DECODED_IMAGE_CHANNEL_COUNT;
        for (uint32 x = 0; x < width; ++x)
        {
            const usize left = usize{x} * 2 * DECODED_IMAGE_CHANNEL_COUNT;
            const usize right = usize{math::Min(x * 2 + 1, inoutImage.Width - 1)} * DECODED_IMAGE_CHANNEL_COUNT;
            const foundation::StaticArray<const uint8*, FILTER_SAMPLE_COUNT> samples{
                {top + left, top + right, bottom + left, bottom + right}};
            uint8* destination = texels.GetData() + (usize{y} * width + x) * DECODED_IMAGE_CHANNEL_COUNT;
            for (usize channel = 0; channel < DECODED_IMAGE_CHANNEL_COUNT; ++channel)
            {
                if (profile == ImageReductionProfile::SrgbColor && channel < COLOR_CHANNEL_COUNT)
                {
                    float32 linear = 0;
                    for (const uint8* sample : samples)
                    {
                        linear += linearChannels[sample[channel]];
                    }
                    destination[channel] = EncodeUnitChannel(math::EncodeSrgb(linear * FILTER_WEIGHT));
                }
                else
                {
                    uint32 sum = 0;
                    for (const uint8* sample : samples)
                    {
                        sum += sample[channel];
                    }
                    destination[channel] = static_cast<uint8>((sum + FILTER_SAMPLE_COUNT / 2) / FILTER_SAMPLE_COUNT);
                }
            }
            if (profile == ImageReductionProfile::TangentNormal)
            {
                math::Vector3 sumNormalTangent{};
                for (const uint8* sample : samples)
                {
                    const float32 xTangent = 2 * static_cast<float32>(sample[0]) / CHANNEL_MAXIMUM - 1;
                    const float32 yTangent = 2 * static_cast<float32>(sample[1]) / CHANNEL_MAXIMUM - 1;
                    const float32 zTangent = math::Sqrt(math::Max(0.0F, 1 - xTangent * xTangent - yTangent * yTangent));
                    math::Vector3 normalTangent;
                    if (math::TryNormalize({xTangent, yTangent, zTangent}, normalTangent) == math::MathStatus::Success)
                    {
                        sumNormalTangent = sumNormalTangent + normalTangent;
                    }
                }
                math::Vector3 normalTangent{0, 0, 1};
                static_cast<void>(math::TryNormalize(sumNormalTangent, normalTangent));
                destination[0] = EncodeUnitChannel(normalTangent.X * 0.5F + 0.5F);
                destination[1] = EncodeUnitChannel(normalTangent.Y * 0.5F + 0.5F);
            }
        }
    }
    inoutImage.Texels.Swap(texels);
    inoutImage.Width = width;
    inoutImage.Height = height;
    return true;
}
} // namespace ludus::content::scene_cook::importers
