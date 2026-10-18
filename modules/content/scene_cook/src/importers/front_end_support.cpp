#include "front_end_support.h"

namespace ludus::content::scene_cook::importers
{
namespace
{
// ITU-R BT.709-6, "Parameter values for the HDTV standards for production and
// international programme exchange", item 3.2: luminance weights of linear R, G, B.
constexpr float64 REC709_RED_WEIGHT = .2126;
constexpr float64 REC709_GREEN_WEIGHT = .7152;
constexpr float64 REC709_BLUE_WEIGHT = .0722;
/// Radix of the decimal digits AppendUnsigned writes.
constexpr foundation::core::uint64 DECIMAL_RADIX = 10;
} // namespace

bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

float64 ComputeRec709Luminance(const float64 (&rgb)[IR_RGB_CHANNEL_COUNT]) noexcept
{
    return REC709_RED_WEIGHT * rgb[0] + REC709_GREEN_WEIGHT * rgb[1] + REC709_BLUE_WEIGHT * rgb[2];
}

MessageBuilder& MessageBuilder::Append(std::string_view text) noexcept
{
    for (const char value : text)
    {
        if (mLength == CAPACITY)
        {
            break;
        }
        mText[mLength++] = value;
    }
    return *this;
}

MessageBuilder& MessageBuilder::AppendUnsigned(foundation::core::uint64 value) noexcept
{
    constexpr usize MAX_DIGITS = 20;
    foundation::StaticArray<char, MAX_DIGITS> digits{};
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % DECIMAL_RADIX);
        value /= DECIMAL_RADIX;
    } while (value != 0 && count < MAX_DIGITS);
    while (count > 0)
    {
        --count;
        (void)Append(std::string_view(&digits[count], 1));
    }
    return *this;
}

bool AddSourceFile(SceneIr& inoutScene, std::string_view relative, const Digest& digest) noexcept
{
    String name;
    return name.TryAssign(relative) == foundation::StringStatus::Ok &&
           inoutScene.SourceFiles.TryAdd(static_cast<String&&>(name)) == true &&
           inoutScene.SourceDigests.TryAdd(digest) == true;
}

bool AddDataFile(SceneIr& inoutScene, std::string_view relative, const Digest& digest) noexcept
{
    String name;
    return name.TryAssign(relative) == foundation::StringStatus::Ok &&
           inoutScene.DataFiles.TryAdd(static_cast<String&&>(name)) == true &&
           inoutScene.DataDigests.TryAdd(digest) == true;
}

void ApplyMaterialEmission(const IrMaterial& material, bool isTwoSided, IrShape& inoutShape) noexcept
{
    const float64 luminance = ComputeRec709Luminance(material.Emission);
    if (luminance <= 0.0)
    {
        return;
    }
    for (usize k = 0; k < IR_RGB_CHANNEL_COUNT; ++k)
    {
        inoutShape.Emission[k] = material.Emission[k];
    }
    inoutShape.EmissionScale = luminance;
    inoutShape.IsEmissionTwoSided = isTwoSided;
    inoutShape.IsEmissionOptional = true;
}
} // namespace ludus::content::scene_cook::importers
