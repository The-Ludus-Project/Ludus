// Image decoder tests against independently encoded fixtures (Pillow JPEGs,
// OpenEXR-library EXRs, Ward-format RGBE); see fixtures/generate_image_fixtures.py.

#include "import_test_support.h"

#include "importers/image/exr_reader.h"
#include "importers/image/image_reader.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
using namespace ludus::scene_import_tests;
using namespace ludus::content::scene_cook::importers;
using ludus::foundation::core::float32;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

constexpr usize FIXTURE_WIDTH = 8;
constexpr usize FIXTURE_HEIGHT = 4;

/// Mirrors linear_value() in generate_image_fixtures.py.
float32 GetExpected(usize x, usize y, usize channel)
{
    if (x == FIXTURE_WIDTH - 1 && y == FIXTURE_HEIGHT - 1)
    {
        const float32 hot[3] = {100.0F, 50.0F, 25.0F};
        return hot[channel];
    }
    const float32 values[3] = {static_cast<float32>(x) / 8.0F,
                               static_cast<float32>(y) / 4.0F,
                               0.25F + static_cast<float32>(channel) * 0.125F};
    return values[channel];
}

void CheckLinearRamp(const DecodedLightingImage& image, float32 relativeTolerance)
{
    REQUIRE(image.Width == FIXTURE_WIDTH);
    REQUIRE(image.Height == FIXTURE_HEIGHT);
    REQUIRE(image.Rgb.GetSize() == FIXTURE_WIDTH * FIXTURE_HEIGHT * 3);
    for (usize y = 0; y < FIXTURE_HEIGHT; ++y)
    {
        for (usize x = 0; x < FIXTURE_WIDTH; ++x)
        {
            for (usize c = 0; c < 3; ++c)
            {
                const float32 expected = GetExpected(x, y, c);
                const float32 actual = image.Rgb[(y * FIXTURE_WIDTH + x) * 3 + c];
                INFO("pixel " << x << "," << y << " channel " << c);
                CHECK(std::fabs(actual - expected) <= relativeTolerance * std::fabs(expected) + 1e-6F);
            }
        }
    }
}
} // namespace

TEST_CASE("OpenEXR subset decodes reference-library files exactly", "[scene_cook][images]")
{
    for (const char* name : {"images/rgb-half-none.exr",
                             "images/rgb-half-rle.exr",
                             "images/rgb-half-zips.exr",
                             "images/rgb-float-zip.exr",
                             "images/rgb-half-tiled-zip.exr"})
    {
        INFO(name);
        const std::string bytes = ReadFileText(GetFixture(name));
        REQUIRE(bytes.empty() == false);
        DecodedLightingImage image;
        std::string_view message;
        REQUIRE(DecodeExr(AsBytes(bytes), image, message) == ImageResult::Ok);
        CheckLinearRamp(image, 0.0F);
        DecodedLightingImage lighting;
        REQUIRE(DecodeLightingImage(name, {AsBytes(bytes).data(), bytes.size()}, lighting, message) == ImageResult::Ok);
        CheckLinearRamp(lighting, 0.0F);
        // Colour textures clamp HDR to [0, 1] and encode sRGB.
        DecodedImage texture;
        REQUIRE(DecodeImage(name, AsBytes(bytes), texture, message) == ImageResult::Ok);
        CHECK(texture.WasHighDynamicRange == true);
        CHECK(texture.Texels[((FIXTURE_HEIGHT - 1) * FIXTURE_WIDTH + FIXTURE_WIDTH - 1) * 4] == 255);
        CHECK(texture.Texels[0] == 0);
    }
}

TEST_CASE("OpenEXR outside the subset is unsupported, corruption is invalid", "[scene_cook][images]")
{
    const std::string piz = ReadFileText(GetFixture("images/rgb-half-piz.exr"));
    DecodedLightingImage image;
    std::string_view message;
    CHECK(DecodeExr(AsBytes(piz), image, message) == ImageResult::Unsupported);
    CHECK(image.Width == 0);

    const std::string zip = ReadFileText(GetFixture("images/rgb-half-zips.exr"));
    for (const std::size_t length : {std::size_t{8}, std::size_t{100}, zip.size() / 2, zip.size() - 1})
    {
        INFO("truncated to " << length);
        CHECK(DecodeExr(AsBytes(std::string_view(zip).substr(0, length)), image, message) == ImageResult::Invalid);
    }
    // Flip bytes inside the last compressed chunk.
    std::string corrupt = zip;
    for (std::size_t i = corrupt.size() - 6; i < corrupt.size(); ++i)
    {
        corrupt[i] = static_cast<char>(corrupt[i] ^ 0x5a);
    }
    CHECK(DecodeExr(AsBytes(corrupt), image, message) == ImageResult::Invalid);
    CHECK(DecodeExr(AsBytes("not an exr"), image, message) == ImageResult::Invalid);
}

TEST_CASE("Radiance RGBE keeps linear energy within RGBE precision", "[scene_cook][images]")
{
    const std::string bytes = ReadFileText(GetFixture("images/ramp.hdr"));
    DecodedLightingImage image;
    std::string_view message;
    REQUIRE(DecodeLightingImage("ramp.hdr", {AsBytes(bytes).data(), bytes.size()}, image, message) == ImageResult::Ok);
    // An 8-bit shared-exponent mantissa resolves about 1/128 of the largest channel.
    CheckLinearRamp(image, 1.0F / 64.0F);
    DecodedImage texture;
    REQUIRE(DecodeImage("ramp.hdr", AsBytes(bytes), texture, message) == ImageResult::Ok);
    CHECK(texture.Width == FIXTURE_WIDTH);
    CHECK(DecodeLightingImage("bad.hdr", {AsBytes("#?RADIANCE\nFORMAT=").data(), 18}, image, message) ==
          ImageResult::Invalid);
}

TEST_CASE("Companion lighting images use the caller's retained byte budget", "[scene_cook][images][environment]")
{
    const std::string bytes = ReadFileText(GetFixture("images/ramp.hdr"));
    constexpr usize SOURCE_PIXELS = FIXTURE_WIDTH * FIXTURE_HEIGHT;
    constexpr usize SOURCE_BYTES = SOURCE_PIXELS * LIGHTING_IMAGE_CHANNEL_COUNT * sizeof(float32);
    constexpr usize REDUCED_BYTES = SOURCE_BYTES / 4;
    DecodedLightingImage full;
    DecodedLightingImage reduced;
    std::string_view message;
    REQUIRE(DecodeLightingImage("ramp.hdr", {AsBytes(bytes).data(), bytes.size()}, full, message, true, SOURCE_BYTES) ==
            ImageResult::Ok);
    REQUIRE(
        DecodeLightingImage("ramp.hdr", {AsBytes(bytes).data(), bytes.size()}, reduced, message, true, REDUCED_BYTES) ==
        ImageResult::Ok);
    CHECK(full.Width == FIXTURE_WIDTH);
    CHECK(full.Height == FIXTURE_HEIGHT);
    CHECK(reduced.Width == FIXTURE_WIDTH / 2);
    CHECK(reduced.Height == FIXTURE_HEIGHT / 2);
    for (usize channel = 0; channel < LIGHTING_IMAGE_CHANNEL_COUNT; ++channel)
    {
        ludus::foundation::float64 fullSum = 0;
        ludus::foundation::float64 reducedSum = 0;
        for (usize sample = channel; sample < full.Rgb.GetSize(); sample += LIGHTING_IMAGE_CHANNEL_COUNT)
        {
            fullSum += static_cast<ludus::foundation::float64>(full.Rgb[sample]);
        }
        for (usize sample = channel; sample < reduced.Rgb.GetSize(); sample += LIGHTING_IMAGE_CHANNEL_COUNT)
        {
            reducedSum += static_cast<ludus::foundation::float64>(reduced.Rgb[sample]);
        }
        CHECK(std::abs(fullSum - reducedSum * 4) < 1e-5);
    }
    CHECK(DecodeLightingImage("ramp.hdr", {AsBytes(bytes).data(), bytes.size()}, reduced, message, true, 0) ==
          ImageResult::Invalid);
    CHECK(reduced.Width == FIXTURE_WIDTH / 2);
}

TEST_CASE("JPEG baseline and progressive match an independent decoder", "[scene_cook][images]")
{
    // Reference pixels decoded by Pillow; IDCT rounding may differ by a few levels.
    struct Probe
    {
        uint32 X;
        uint32 Y;
        int Rgb[3];
    };
    const Probe probes[] = {{0, 0, {0, 0, 127}}, {15, 7, {239, 225, 128}}, {8, 4, {128, 128, 126}}};
    for (const char* name : {"images/gradient-baseline.jpg", "images/gradient-progressive.jpg"})
    {
        INFO(name);
        const std::string bytes = ReadFileText(GetFixture(name));
        DecodedImage image;
        std::string_view message;
        REQUIRE(DecodeImage(name, AsBytes(bytes), image, message) == ImageResult::Ok);
        REQUIRE(image.Width == 16);
        REQUIRE(image.Height == 8);
        CHECK(image.WasHighDynamicRange == false);
        for (const Probe& probe : probes)
        {
            for (uint32 c = 0; c < 3; ++c)
            {
                const int actual = image.Texels[(probe.Y * 16 + probe.X) * 4 + c];
                CHECK(std::abs(actual - probe.Rgb[c]) <= 4);
            }
            CHECK(image.Texels[(probe.Y * 16 + probe.X) * 4 + 3] == 255);
        }
        // Detection is by content: a misleading extension does not matter.
        DecodedImage renamed;
        CHECK(DecodeImage("texture.png", AsBytes(bytes), renamed, message) == ImageResult::Ok);
        CHECK(DecodeImage(name, AsBytes(std::string_view(bytes).substr(0, 40)), image, message) ==
              ImageResult::Invalid);
    }
}
