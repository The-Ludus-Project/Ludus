// Texture viewer CPU rules: texel decoding per encoding, probe text, channel selection, range
// remapping, exposure and the nearest-neighbour view transform. No GPU.
#include "internal/texture_inspection.h"

#include <ludus/foundation/base/types.h>

#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QColor>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
/// binary16 encodings of 1, 2, -2, 65504 (largest finite), the smallest subnormal, +Inf and a NaN.
constexpr uint16 HALF_ONE = 0x3C00;
constexpr uint16 HALF_TWO = 0x4000;
constexpr uint16 HALF_MINUS_TWO = 0xC000;
constexpr uint16 HALF_MAXIMUM = 0x7BFF;
constexpr uint16 HALF_SMALLEST = 0x0001;
constexpr uint16 HALF_INFINITY = 0x7C00;
constexpr uint16 HALF_NAN = 0x7E00;
constexpr float32 HALF_MAXIMUM_VALUE = 65504.0F;
constexpr int32 SMALLEST_SUBNORMAL_EXPONENT = -24;
constexpr uint32 BYTE_BITS = 8;
constexpr uint8 BYTE_MASK = 0xFF;

InspectedTexture MakeRgba8(uint32 width, uint32 height, std::initializer_list<uint8> bytes)
{
    InspectedTexture texture;
    texture.Encoding = TexelEncoding::Rgba8Unorm;
    texture.Width = width;
    texture.Height = height;
    for (const uint8 byte : bytes)
    {
        texture.Bytes.append(static_cast<char>(byte));
    }
    return texture;
}

InspectedTexture MakeDepth(uint32 width, uint32 height, std::initializer_list<float32> values)
{
    InspectedTexture texture;
    texture.Encoding = TexelEncoding::Depth32Float;
    texture.Width = width;
    texture.Height = height;
    for (const float32 value : values)
    {
        const uint32 bits = std::bit_cast<uint32>(value);
        for (uint32 i = 0; i < sizeof(uint32); ++i)
        {
            texture.Bytes.append(static_cast<char>((bits >> (BYTE_BITS * i)) & BYTE_MASK));
        }
    }
    return texture;
}

InspectedTexture MakeHalf(uint32 width, uint32 height, std::initializer_list<uint16> halves)
{
    InspectedTexture texture;
    texture.Encoding = TexelEncoding::Rgba16Float;
    texture.Width = width;
    texture.Height = height;
    for (const uint16 half : halves)
    {
        texture.Bytes.append(static_cast<char>(half & BYTE_MASK));
        texture.Bytes.append(static_cast<char>(half >> BYTE_BITS));
    }
    return texture;
}
} // namespace

TEST_CASE("Texture viewer decodes binary16 and binary32 exactly", "[editor][texture-viewer]")
{
    CHECK(DecodeHalf(HALF_ONE) == 1.0F);
    CHECK(DecodeHalf(HALF_TWO) == 2.0F);
    CHECK(DecodeHalf(HALF_MINUS_TWO) == -2.0F);
    CHECK(DecodeHalf(HALF_MAXIMUM) == HALF_MAXIMUM_VALUE);
    CHECK(DecodeHalf(HALF_SMALLEST) == std::ldexp(1.0F, SMALLEST_SUBNORMAL_EXPONENT));
    CHECK(DecodeHalf(0) == 0.0F);
    CHECK(std::isinf(DecodeHalf(HALF_INFINITY)) == true);
    CHECK(std::isnan(DecodeHalf(HALF_NAN)) == true);
    // 0.5 is 0x3F000000: little-endian bytes 00 00 00 3F.
    CHECK(DecodeFloat(0, 0, 0, 0x3F) == 0.5F);
    const float32 awkward = 0.123456789F;
    const uint32 bits = std::bit_cast<uint32>(awkward);
    CHECK(DecodeFloat(static_cast<uint8>(bits & BYTE_MASK),
                      static_cast<uint8>((bits >> BYTE_BITS) & BYTE_MASK),
                      static_cast<uint8>((bits >> (BYTE_BITS * 2)) & BYTE_MASK),
                      static_cast<uint8>(bits >> (BYTE_BITS * 3))) == awkward);
}

TEST_CASE("Texture viewer reads texels and describes them in their format", "[editor][texture-viewer]")
{
    const InspectedTexture color = MakeRgba8(2, 1, {255, 128, 0, 255, 10, 20, 30, 40});
    REQUIRE(color.IsValid() == true);
    TexelValue value;
    REQUIRE(ReadTexel(color, 1, 0, value) == true);
    CHECK(value.ChannelCount == MAX_TEXEL_CHANNELS);
    CHECK(value.Bits[0] == 10);
    CHECK(value.Bits[3] == 40);
    CHECK(value.Channels[2] == Catch::Approx(30.0 / 255.0));
    CHECK(ReadTexel(color, 2, 0, value) == false);
    CHECK(DescribeTexel(color, 0, 0) ==
          QStringLiteral("(0, 0)  R 255 (1.0000)  G 128 (0.5020)  B 0 (0.0000)  A 255 (1.0000)"));
    CHECK(DescribeTexel(color, 0, 1).isEmpty() == true);

    const InspectedTexture depth = MakeDepth(1, 2, {0.5F, 1.0F});
    REQUIRE(ReadTexel(depth, 0, 0, value) == true);
    CHECK(value.ChannelCount == 1);
    CHECK(value.Channels[0] == 0.5F);
    CHECK(value.Bits[0] == 0x3F000000U);
    CHECK(DescribeTexel(depth, 0, 0) == QStringLiteral("(0, 0)  depth 0.5 (0x3f000000)"));
    CHECK(DescribeTexel(depth, 0, 1) == QStringLiteral("(0, 1)  depth 1 (0x3f800000)"));

    const InspectedTexture half = MakeHalf(1, 1, {HALF_TWO, HALF_ONE, 0, HALF_ONE});
    REQUIRE(ReadTexel(half, 0, 0, value) == true);
    CHECK(value.Channels[0] == 2.0F);
    CHECK(value.Bits[0] == HALF_TWO);
    CHECK(DescribeTexel(half, 0, 0).startsWith(QStringLiteral("(0, 0)  R 2 (0x4000)  G 1 (0x3c00)")) == true);

    InspectedTexture truncated = color;
    truncated.Bytes.chop(1);
    CHECK(truncated.IsValid() == false);
    CHECK(ReadTexel(truncated, 0, 0, value) == false);
}

TEST_CASE("Texture viewer channel, remap and range rules", "[editor][texture-viewer]")
{
    TexelValue value;
    value.Channels = {{0.25F, 0.5F, 0.75F, 1.0F}};
    CHECK(SelectChannel(value, TextureChannel::Red) == 0.25F);
    CHECK(SelectChannel(value, TextureChannel::Green) == 0.5F);
    CHECK(SelectChannel(value, TextureChannel::Blue) == 0.75F);
    CHECK(SelectChannel(value, TextureChannel::Alpha) == 1.0F);
    CHECK(SelectChannel(value, TextureChannel::Luminance) ==
          Catch::Approx(0.2126 * 0.25 + 0.7152 * 0.5 + 0.0722 * 0.75));

    CHECK(Remap(0.5F, 0.0F, 1.0F) == 0.5F);
    CHECK(Remap(0.75F, 0.5F, 1.0F) == 0.5F);
    CHECK(Remap(-1.0F, 0.0F, 1.0F) == 0.0F);
    CHECK(Remap(2.0F, 0.0F, 1.0F) == 1.0F);
    CHECK(Remap(std::numeric_limits<float32>::infinity(), 0.0F, 1.0F) == 1.0F);
    CHECK(Remap(std::numeric_limits<float32>::quiet_NaN(), 0.0F, 1.0F) == 0.0F);
    // An empty range is a step, never a division by zero.
    CHECK(Remap(0.5F, 0.5F, 0.5F) == 0.0F);
    CHECK(Remap(0.75F, 0.5F, 0.5F) == 1.0F);

    // Depth background (exactly 1) is left out of the automatic range unless asked for.
    const InspectedTexture depth = MakeDepth(2, 2, {0.25F, 0.75F, 1.0F, 0.5F});
    TextureValueRange range = FindValueRange(depth, TextureChannel::Rgba, true);
    CHECK(range.HasValues == true);
    CHECK(range.Minimum == 0.25F);
    CHECK(range.Maximum == 0.75F);
    range = FindValueRange(depth, TextureChannel::Rgba, false);
    CHECK(range.Maximum == 1.0F);
    CHECK(FindValueRange(MakeDepth(1, 1, {1.0F}), TextureChannel::Red, true).HasValues == false);
    // Rgba spans R, G and B only; alpha is its own channel.
    const InspectedTexture color = MakeRgba8(1, 1, {51, 102, 204, 0});
    range = FindValueRange(color, TextureChannel::Rgba, true);
    CHECK(range.Minimum == Catch::Approx(0.2));
    CHECK(range.Maximum == Catch::Approx(0.8));
    CHECK(FindValueRange(color, TextureChannel::Alpha, true).Maximum == 0.0F);

    CHECK(IsChannelAvailable(TexelEncoding::Depth32Float, TextureChannel::Red) == true);
    CHECK(IsChannelAvailable(TexelEncoding::Depth32Float, TextureChannel::Green) == false);
    CHECK(IsChannelAvailable(TexelEncoding::Rgba8Unorm, TextureChannel::Alpha) == true);
    CHECK(IsHighDynamicRange(TexelEncoding::Rgba16Float) == true);
    CHECK(IsHighDynamicRange(TexelEncoding::Rgba8Unorm) == false);
    CHECK(IsHighDynamicRange(TexelEncoding::Depth32Float) == false);
}

TEST_CASE("Texture viewer display image applies channel, range and exposure", "[editor][texture-viewer]")
{
    TextureValueRange applied;
    // Depth remapped to its automatic range: nearest is black, farthest surface white, background clamps.
    const InspectedTexture depth = MakeDepth(3, 1, {0.25F, 0.75F, 1.0F});
    TextureDisplaySettings settings;
    settings.Range = TextureRangeMode::Automatic;
    QImage image = MakeDisplayImage(depth, settings, applied);
    REQUIRE(image.size() == QSize(3, 1));
    CHECK(applied.Minimum == 0.25F);
    CHECK(applied.Maximum == 0.75F);
    CHECK(image.pixelColor(0, 0) == QColor(0, 0, 0));
    CHECK(image.pixelColor(1, 0) == QColor(255, 255, 255));
    CHECK(image.pixelColor(2, 0) == QColor(255, 255, 255));
    // A depth texture asked for green falls back to its single channel.
    settings.Channel = TextureChannel::Green;
    CHECK(MakeDisplayImage(depth, settings, applied).pixelColor(1, 0) == QColor(255, 255, 255));

    // Single colour channels show as grey; alpha never makes the display transparent.
    const InspectedTexture color = MakeRgba8(1, 1, {255, 128, 0, 64});
    settings = {};
    settings.Channel = TextureChannel::Green;
    image = MakeDisplayImage(color, settings, applied);
    CHECK(image.pixelColor(0, 0) == QColor(128, 128, 128, 255));
    settings.Channel = TextureChannel::Alpha;
    CHECK(MakeDisplayImage(color, settings, applied).pixelColor(0, 0) == QColor(64, 64, 64, 255));
    settings.Channel = TextureChannel::Rgba;
    CHECK(MakeDisplayImage(color, settings, applied).pixelColor(0, 0) == QColor(255, 128, 0, 255));
    // Manual range [0.5, 1] maps 128/255 to almost black.
    settings.Minimum = 0.5F;
    CHECK(MakeDisplayImage(color, settings, applied).pixelColor(0, 0).green() == 1);
    // Exposure has no effect on an LDR encoding.
    settings = {};
    settings.ExposureStops = -1;
    CHECK(MakeDisplayImage(color, settings, applied).pixelColor(0, 0) == QColor(255, 128, 0, 255));

    // HDR: 2.0 is clipped at stop 0 and mid grey (to rounding) at -2 stops.
    const InspectedTexture half = MakeHalf(2, 1, {HALF_TWO, HALF_TWO, HALF_TWO, HALF_ONE, HALF_NAN, 0, 0, HALF_ONE});
    settings = {};
    CHECK(MakeDisplayImage(half, settings, applied).pixelColor(0, 0) == QColor(255, 255, 255));
    settings.ExposureStops = -2;
    image = MakeDisplayImage(half, settings, applied);
    CHECK(image.pixelColor(0, 0) == QColor(128, 128, 128));
    // Non-finite texels stand out in magenta.
    CHECK(image.pixelColor(1, 0) == QColor(255, 0, 255));

    CHECK(MakeDisplayImage(InspectedTexture{}, settings, applied).isNull() == true);
}

TEST_CASE("Texture viewer zoom, pan and probe mapping stay on texel centres", "[editor][texture-viewer]")
{
    // A 96 x 64 texture in a 400 x 300 view fits at whole zoom 4, centred.
    TextureViewTransform transform = FitTexture(96, 64, 400, 300);
    CHECK(transform.Zoom == 4.0F);
    CHECK(transform.OffsetX == 8.0F);
    CHECK(transform.OffsetY == 22.0F);
    uint32 x = 0;
    uint32 y = 0;
    REQUIRE(MapToTexel(transform, 96, 64, 8.0F, 22.0F, x, y) == true);
    CHECK((x == 0 && y == 0));
    REQUIRE(MapToTexel(transform, 96, 64, 11.9F, 25.9F, x, y) == true);
    CHECK((x == 0 && y == 0));
    REQUIRE(MapToTexel(transform, 96, 64, 12.0F, 26.0F, x, y) == true);
    CHECK((x == 1 && y == 1));
    CHECK(MapToTexel(transform, 96, 64, 7.9F, 22.0F, x, y) == false);
    CHECK(MapToTexel(transform, 96, 64, 392.0F, 22.0F, x, y) == false);
    // A texture larger than the view fits at a fraction.
    CHECK(FitTexture(1280, 720, 640, 360).Zoom == 0.5F);

    // Zooming keeps the texel under the cursor in place.
    const TextureViewTransform zoomed = ZoomTexture(transform, 2.0F, 50.0F, 50.0F);
    CHECK(zoomed.Zoom == 8.0F);
    uint32 before = 0;
    uint32 after = 0;
    uint32 row = 0;
    REQUIRE(MapToTexel(transform, 96, 64, 50.0F, 50.0F, before, row) == true);
    REQUIRE(MapToTexel(zoomed, 96, 64, 50.0F, 50.0F, after, row) == true);
    CHECK(before == after);
    CHECK(ZoomTexture(transform, 1e6F, 0, 0).Zoom == MAXIMUM_TEXTURE_ZOOM);
    CHECK(ZoomTexture(transform, 1e-6F, 0, 0).Zoom == MINIMUM_TEXTURE_ZOOM);
    CHECK(ZoomTexture(transform, 0, 0, 0).Zoom == transform.Zoom);
}
