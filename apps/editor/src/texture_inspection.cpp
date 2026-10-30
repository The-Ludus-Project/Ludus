#include "internal/texture_inspection.h"

#include <ludus/foundation/math/scalar.hpp>

#include <atomic>
#include <bit>
#include <cmath>
#include <limits>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr uint32 RGBA8_TEXEL_BYTES = 4;
constexpr uint32 RGBA16_TEXEL_BYTES = 8;
constexpr uint32 DEPTH32_TEXEL_BYTES = 4;
constexpr uint32 BITS_PER_BYTE = 8;
constexpr uint32 HALF_BYTES = 2;
/// Largest 8-bit normalized code, which encodes one.
constexpr float32 UNORM8_MAXIMUM = 255.0F;
/// Rec. 709 relative luminance weights of linear R, G and B.
constexpr float32 LUMINANCE_RED = 0.2126F;
constexpr float32 LUMINANCE_GREEN = 0.7152F;
constexpr float32 LUMINANCE_BLUE = 0.0722F;
/// IEEE binary16 layout.
constexpr uint32 HALF_SIGN_SHIFT = 15;
constexpr uint32 HALF_EXPONENT_SHIFT = 10;
constexpr uint32 HALF_EXPONENT_MASK = 0x1F;
constexpr uint32 HALF_MANTISSA_MASK = 0x3FF;
constexpr int32 HALF_EXPONENT_BIAS = 15;
/// 2^-14 (smallest normal exponent) and 2^-10 (one mantissa step) give subnormal values.
constexpr int32 HALF_SUBNORMAL_EXPONENT = -14;
constexpr int32 HALF_MANTISSA_BITS = 10;
/// The cleared far plane of the conventional depth convention.
constexpr float32 DEPTH_BACKGROUND = 1.0F;
/// Probe text digits: depth needs every binary32 digit to tell nearby surfaces apart.
constexpr int32 DEPTH_DIGITS = 9;
constexpr int32 COLOUR_DIGITS = 4;
constexpr int32 FLOAT_DIGITS = 6;
constexpr int32 HEX_BASE = 16;
constexpr int32 HALF_HEX_DIGITS = 4;
constexpr int32 FLOAT_HEX_DIGITS = 8;
/// Non-finite texels draw in magenta (straight RGBA8).
constexpr uint8 INVALID_RED = 255;
constexpr uint8 INVALID_GREEN = 0;
constexpr uint8 INVALID_BLUE = 255;
constexpr uint8 OPAQUE = 255;

uint16 ReadHalf(const uint8* bytes) noexcept
{
    return static_cast<uint16>(static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << BITS_PER_BYTE));
}

uint8 EncodeUnorm(float32 value) noexcept
{
    return static_cast<uint8>(std::lround(math::Clamp(value, 0.0F, 1.0F) * UNORM8_MAXIMUM));
}

bool IsShown(TexelEncoding encoding, float32 value, bool shouldIgnoreDepthBackground) noexcept
{
    return math::IsFinite(value) == true && (encoding != TexelEncoding::Depth32Float ||
                                             shouldIgnoreDepthBackground == false || value != DEPTH_BACKGROUND);
}

const char* GetChannelName(TexelEncoding encoding, uint32 channel) noexcept
{
    static constexpr const char* NAMES[MAX_TEXEL_CHANNELS] = {"R", "G", "B", "A"};
    return encoding == TexelEncoding::Depth32Float ? "depth" : NAMES[channel];
}
} // namespace

bool InspectedTexture::IsValid() const noexcept
{
    return Width != 0 && Height != 0 &&
           static_cast<uint64>(Bytes.size()) ==
               static_cast<uint64>(Width) * static_cast<uint64>(Height) * GetTexelBytes(Encoding);
}

uint32 GetTexelBytes(TexelEncoding encoding) noexcept
{
    switch (encoding)
    {
        case TexelEncoding::Rgba8Unorm:
            return RGBA8_TEXEL_BYTES;
        case TexelEncoding::Rgba16Float:
            return RGBA16_TEXEL_BYTES;
        case TexelEncoding::Depth32Float:
            return DEPTH32_TEXEL_BYTES;
    }
    return 0;
}

uint32 GetChannelCount(TexelEncoding encoding) noexcept
{
    return encoding == TexelEncoding::Depth32Float ? 1 : MAX_TEXEL_CHANNELS;
}

bool IsHighDynamicRange(TexelEncoding encoding) noexcept
{
    return encoding == TexelEncoding::Rgba16Float;
}

bool IsChannelAvailable(TexelEncoding encoding, TextureChannel channel) noexcept
{
    return encoding != TexelEncoding::Depth32Float || channel == TextureChannel::Rgba || channel == TextureChannel::Red;
}

QString GetEncodingName(TexelEncoding encoding)
{
    switch (encoding)
    {
        case TexelEncoding::Rgba8Unorm:
            return QStringLiteral("RGBA8Unorm");
        case TexelEncoding::Rgba16Float:
            return QStringLiteral("RGBA16Float");
        case TexelEncoding::Depth32Float:
            return QStringLiteral("Depth32Float");
    }
    return QStringLiteral("Unknown");
}

float32 DecodeHalf(uint16 bits) noexcept
{
    const uint32 word = bits;
    const float32 sign = (word >> HALF_SIGN_SHIFT) == 0 ? 1.0F : -1.0F;
    const uint32 exponent = (word >> HALF_EXPONENT_SHIFT) & HALF_EXPONENT_MASK;
    const uint32 mantissa = word & HALF_MANTISSA_MASK;
    if (exponent == HALF_EXPONENT_MASK)
    {
        return mantissa == 0 ? sign * std::numeric_limits<float32>::infinity()
                             : std::numeric_limits<float32>::quiet_NaN();
    }
    if (exponent == 0)
    {
        return sign * std::ldexp(static_cast<float32>(mantissa), HALF_SUBNORMAL_EXPONENT - HALF_MANTISSA_BITS);
    }
    const float32 significand = 1.0F + std::ldexp(static_cast<float32>(mantissa), -HALF_MANTISSA_BITS);
    return sign * std::ldexp(significand, static_cast<int32>(exponent) - HALF_EXPONENT_BIAS);
}

// Bytes are named by little-endian significance.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float32 DecodeFloat(uint8 byte0, uint8 byte1, uint8 byte2, uint8 byte3) noexcept
{
    const uint32 bits = static_cast<uint32>(byte0) | (static_cast<uint32>(byte1) << BITS_PER_BYTE) |
                        (static_cast<uint32>(byte2) << (BITS_PER_BYTE * 2)) |
                        (static_cast<uint32>(byte3) << (BITS_PER_BYTE * (RGBA8_TEXEL_BYTES - 1)));
    return std::bit_cast<float32>(bits);
}

bool ReadTexel(const InspectedTexture& texture, uint32 x, uint32 y, TexelValue& outValue) noexcept
{
    if (texture.IsValid() == false || x >= texture.Width || y >= texture.Height)
    {
        return false;
    }
    const uint32 texelBytes = GetTexelBytes(texture.Encoding);
    const uint8* bytes = reinterpret_cast<const uint8*>(texture.Bytes.constData()) +
                         (static_cast<usize>(y) * texture.Width + x) * texelBytes;
    TexelValue value;
    value.ChannelCount = GetChannelCount(texture.Encoding);
    switch (texture.Encoding)
    {
        case TexelEncoding::Rgba8Unorm:
            for (uint32 channel = 0; channel < MAX_TEXEL_CHANNELS; ++channel)
            {
                value.Bits[channel] = bytes[channel];
                value.Channels[channel] = static_cast<float32>(bytes[channel]) / UNORM8_MAXIMUM;
            }
            break;
        case TexelEncoding::Rgba16Float:
            for (uint32 channel = 0; channel < MAX_TEXEL_CHANNELS; ++channel)
            {
                const uint16 half = ReadHalf(bytes + static_cast<usize>(channel) * HALF_BYTES);
                value.Bits[channel] = half;
                value.Channels[channel] = DecodeHalf(half);
            }
            break;
        case TexelEncoding::Depth32Float:
            value.Channels[0] = DecodeFloat(bytes[0], bytes[1], bytes[2], bytes[RGBA8_TEXEL_BYTES - 1]);
            value.Bits[0] = std::bit_cast<uint32>(value.Channels[0]);
            break;
    }
    outValue = value;
    return true;
}

float32 SelectChannel(const TexelValue& value, TextureChannel channel) noexcept
{
    switch (channel)
    {
        case TextureChannel::Rgba:
        case TextureChannel::Red:
            return value.Channels[0];
        case TextureChannel::Green:
            return value.Channels[1];
        case TextureChannel::Blue:
            return value.Channels[2];
        case TextureChannel::Alpha:
            return value.Channels[MAX_TEXEL_CHANNELS - 1];
        case TextureChannel::Luminance:
            return LUMINANCE_RED * value.Channels[0] + LUMINANCE_GREEN * value.Channels[1] +
                   LUMINANCE_BLUE * value.Channels[2];
    }
    return 0;
}

float32 Remap(float32 value, float32 minimum, float32 maximum) noexcept
{
    if (std::isnan(value) == true)
    {
        return 0;
    }
    if (maximum <= minimum)
    {
        return value > minimum ? 1.0F : 0.0F;
    }
    const float32 mapped = (value - minimum) / (maximum - minimum);
    // An infinite value over a finite range stays ordered; only NaN needs the guard above.
    return std::isnan(mapped) == true ? 0.0F : math::Clamp(mapped, 0.0F, 1.0F);
}

void TextureConversionCancellation::Cancel() noexcept
{
    mIsCancelled.store(true, std::memory_order_relaxed);
}

bool TextureConversionCancellation::IsCancelled() const noexcept
{
    return mIsCancelled.load(std::memory_order_relaxed);
}

void TextureConversionCancellation::Reset() noexcept
{
    mIsCancelled.store(false, std::memory_order_relaxed);
}

TextureValueRange FindValueRange(const InspectedTexture& texture,
                                 TextureChannel channel,
                                 bool shouldIgnoreDepthBackground,
                                 const TextureConversionCancellation* cancellationOrNull) noexcept
{
    TextureValueRange range;
    if (texture.IsValid() == false)
    {
        return range;
    }
    // Rgba spans the colour channels; alpha is a separate quantity and stays out.
    const bool isColor = channel == TextureChannel::Rgba && texture.Encoding != TexelEncoding::Depth32Float;
    const uint32 colorChannels = MAX_TEXEL_CHANNELS - 1;
    for (uint32 y = 0; y < texture.Height; ++y)
    {
        if (cancellationOrNull != nullptr && cancellationOrNull->IsCancelled() == true)
        {
            return {};
        }
        for (uint32 x = 0; x < texture.Width; ++x)
        {
            TexelValue value;
            static_cast<void>(ReadTexel(texture, x, y, value));
            const uint32 count = isColor == true ? colorChannels : 1;
            for (uint32 i = 0; i < count; ++i)
            {
                const float32 shown = isColor == true ? value.Channels[i] : SelectChannel(value, channel);
                if (IsShown(texture.Encoding, shown, shouldIgnoreDepthBackground) == false)
                {
                    continue;
                }
                if (range.HasValues == false)
                {
                    range.Minimum = shown;
                    range.Maximum = shown;
                    range.HasValues = true;
                }
                range.Minimum = shown < range.Minimum ? shown : range.Minimum;
                range.Maximum = shown > range.Maximum ? shown : range.Maximum;
            }
        }
    }
    return range;
}

QString DescribeTexel(const InspectedTexture& texture, uint32 x, uint32 y)
{
    TexelValue value;
    if (ReadTexel(texture, x, y, value) == false)
    {
        return {};
    }
    QString text = QStringLiteral("(%1, %2)").arg(x).arg(y);
    for (uint32 channel = 0; channel < value.ChannelCount; ++channel)
    {
        const QString name = QString::fromLatin1(GetChannelName(texture.Encoding, channel));
        switch (texture.Encoding)
        {
            case TexelEncoding::Rgba8Unorm:
                text += QStringLiteral("  %1 %2 (%3)")
                            .arg(name)
                            .arg(value.Bits[channel])
                            .arg(static_cast<float64>(value.Channels[channel]), 0, 'f', COLOUR_DIGITS);
                break;
            case TexelEncoding::Rgba16Float:
                text += QStringLiteral("  %1 %2 (0x%3)")
                            .arg(name)
                            .arg(static_cast<float64>(value.Channels[channel]), 0, 'g', FLOAT_DIGITS)
                            .arg(value.Bits[channel], HALF_HEX_DIGITS, HEX_BASE, QLatin1Char('0'));
                break;
            case TexelEncoding::Depth32Float:
                text += QStringLiteral("  %1 %2 (0x%3)")
                            .arg(name)
                            .arg(static_cast<float64>(value.Channels[channel]), 0, 'g', DEPTH_DIGITS)
                            .arg(value.Bits[channel], FLOAT_HEX_DIGITS, HEX_BASE, QLatin1Char('0'));
                break;
        }
    }
    return text;
}

QImage MakeDisplayImage(const InspectedTexture& texture,
                        const TextureDisplaySettings& settings,
                        TextureValueRange& outRange,
                        const TextureConversionCancellation* cancellationOrNull)
{
    if (texture.IsValid() == false || (cancellationOrNull != nullptr && cancellationOrNull->IsCancelled() == true))
    {
        return {};
    }
    const TextureChannel channel =
        IsChannelAvailable(texture.Encoding, settings.Channel) == true ? settings.Channel : TextureChannel::Rgba;
    TextureValueRange range;
    if (settings.Range == TextureRangeMode::Automatic)
    {
        range = FindValueRange(texture, channel, settings.ShouldIgnoreDepthBackground, cancellationOrNull);
    }
    else
    {
        range = {settings.Minimum, settings.Maximum, true};
    }
    if (cancellationOrNull != nullptr && cancellationOrNull->IsCancelled() == true)
    {
        return {};
    }
    // Exposure scales colour values before the remap; the automatic range is measured before it.
    const float32 scale = IsHighDynamicRange(texture.Encoding) == true ? std::exp2(settings.ExposureStops) : 1.0F;
    const bool isColor = channel == TextureChannel::Rgba && GetChannelCount(texture.Encoding) == MAX_TEXEL_CHANNELS;
    QImage image(static_cast<int32>(texture.Width), static_cast<int32>(texture.Height), QImage::Format_RGBA8888);
    if (image.isNull() == true)
    {
        return {};
    }
    for (uint32 y = 0; y < texture.Height; ++y)
    {
        if (cancellationOrNull != nullptr && cancellationOrNull->IsCancelled() == true)
        {
            return {};
        }
        uint8* row = image.scanLine(static_cast<int32>(y));
        for (uint32 x = 0; x < texture.Width; ++x)
        {
            TexelValue value;
            static_cast<void>(ReadTexel(texture, x, y, value));
            uint8* pixel = row + static_cast<usize>(x) * RGBA8_TEXEL_BYTES;
            pixel[RGBA8_TEXEL_BYTES - 1] = OPAQUE;
            if (isColor == true)
            {
                for (uint32 i = 0; i < RGBA8_TEXEL_BYTES - 1; ++i)
                {
                    pixel[i] = EncodeUnorm(Remap(value.Channels[i] * scale, range.Minimum, range.Maximum));
                }
                if (math::IsFinite(value.Channels[0] + value.Channels[1] + value.Channels[2]) == false)
                {
                    pixel[0] = INVALID_RED;
                    pixel[1] = INVALID_GREEN;
                    pixel[2] = INVALID_BLUE;
                }
                continue;
            }
            const float32 shown = SelectChannel(value, channel) * (channel == TextureChannel::Alpha ? 1.0F : scale);
            if (math::IsFinite(shown) == false)
            {
                pixel[0] = INVALID_RED;
                pixel[1] = INVALID_GREEN;
                pixel[2] = INVALID_BLUE;
                continue;
            }
            const uint8 grey = EncodeUnorm(Remap(shown, range.Minimum, range.Maximum));
            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;
        }
    }
    outRange = range;
    return image;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — texture and view extents.
TextureViewTransform
FitTexture(uint32 textureWidth, uint32 textureHeight, float32 viewWidth, float32 viewHeight) noexcept
{
    TextureViewTransform transform;
    if (textureWidth == 0 || textureHeight == 0 || viewWidth <= 0 || viewHeight <= 0)
    {
        return transform;
    }
    const float32 fit =
        math::Min(viewWidth / static_cast<float32>(textureWidth), viewHeight / static_cast<float32>(textureHeight));
    const float32 zoom = fit >= 1.0F ? std::floor(fit) : fit;
    transform.Zoom = math::Clamp(zoom, MINIMUM_TEXTURE_ZOOM, MAXIMUM_TEXTURE_ZOOM);
    transform.OffsetX = std::floor((viewWidth - static_cast<float32>(textureWidth) * transform.Zoom) * 0.5F);
    transform.OffsetY = std::floor((viewHeight - static_cast<float32>(textureHeight) * transform.Zoom) * 0.5F);
    return transform;
}

TextureViewTransform
ZoomTexture(const TextureViewTransform& transform, float32 factor, float32 anchorX, float32 anchorY) noexcept
{
    if (factor <= 0 || math::IsFinite(factor) == false)
    {
        return transform;
    }
    TextureViewTransform zoomed = transform;
    zoomed.Zoom = math::Clamp(transform.Zoom * factor, MINIMUM_TEXTURE_ZOOM, MAXIMUM_TEXTURE_ZOOM);
    const float32 applied = zoomed.Zoom / transform.Zoom;
    zoomed.OffsetX = anchorX - (anchorX - transform.OffsetX) * applied;
    zoomed.OffsetY = anchorY - (anchorY - transform.OffsetY) * applied;
    return zoomed;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — extents, then the view position.
bool MapToTexel(const TextureViewTransform& transform,
                uint32 textureWidth,
                uint32 textureHeight,
                float32 viewX,
                float32 viewY,
                uint32& outX,
                uint32& outY) noexcept
{
    if (transform.Zoom <= 0)
    {
        return false;
    }
    const float32 u = std::floor((viewX - transform.OffsetX) / transform.Zoom);
    const float32 v = std::floor((viewY - transform.OffsetY) / transform.Zoom);
    if (u < 0 || v < 0 || u >= static_cast<float32>(textureWidth) || v >= static_cast<float32>(textureHeight))
    {
        return false;
    }
    outX = static_cast<uint32>(u);
    outY = static_cast<uint32>(v);
    return true;
}
} // namespace ludus::editor
