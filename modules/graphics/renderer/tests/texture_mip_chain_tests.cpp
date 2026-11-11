// CPU checks of the RGBA8 minification chain the scene resources upload for material textures.

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/texture_mip_chain.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

namespace
{
constexpr usize CHANNEL_COUNT = 4;
constexpr uint8 CODE_MAXIMUM = 255;
constexpr usize CHECKER_EXTENT = 2;
constexpr usize CHECKER_BYTES = CHECKER_EXTENT * CHECKER_EXTENT * CHANNEL_COUNT;
uint8 EncodeSrgbCode(float32 linear)
{
    return static_cast<uint8>(math::Floor(static_cast<float64>(math::EncodeSrgb(linear)) * CODE_MAXIMUM + 0.5));
}
} // namespace

TEST_CASE("mip level counts follow the largest extent", "[texture-mips]")
{
    CHECK(renderer::CountRgba8MipLevels(1, 1) == 1);
    CHECK(renderer::CountRgba8MipLevels(2, 1) == 2);
    CHECK(renderer::CountRgba8MipLevels(3, 1) == 2);
    CHECK(renderer::CountRgba8MipLevels(4, 7) == 3);
    CHECK(renderer::CountRgba8MipLevels(256, 1) == 9);
    CHECK(renderer::CountRgba8MipLevels(1 << 20, 1) == renderer::MAX_TEXTURE_MIP_LEVELS);
}

TEST_CASE("sRGB checker levels average in linear light while data averages its codes", "[texture-mips]")
{
    // Black and white checks: the mean of their linear light is 0.5, not the sRGB mid code 128.
    StaticArray<uint8, CHECKER_BYTES> checker{};
    for (usize texel = 0; texel < CHECKER_EXTENT * CHECKER_EXTENT; ++texel)
    {
        const uint8 code = (texel == 1 || texel == 2) ? CODE_MAXIMUM : 0;
        checker[texel * CHANNEL_COUNT] = code;
        checker[texel * CHANNEL_COUNT + 1] = code;
        checker[texel * CHANNEL_COUNT + 2] = code;
        checker[texel * CHANNEL_COUNT + 3] = CODE_MAXIMUM;
    }
    rhi::TextureCpuContent source;
    source.Bytes = checker.GetData();
    source.Size = checker.GetSize();
    source.RowPitch = CHECKER_EXTENT * CHANNEL_COUNT;
    source.Width = CHECKER_EXTENT;
    source.Height = CHECKER_EXTENT;

    SECTION("sRGB colour")
    {
        source.Format = rhi::RasterFormat::Rgba8Srgb;
        renderer::Rgba8MipChain chain;
        REQUIRE(renderer::BuildRgba8MipChain(source, 2, chain) == rhi::RasterStatus::Ready);
        REQUIRE(chain.LevelCount == 2);
        REQUIRE(chain.Bytes.GetSize() == CHECKER_BYTES + CHANNEL_COUNT);
        REQUIRE(chain.Mips.GetSize() == 2);
        CHECK(chain.Mips[0].Offset == 0);
        CHECK(chain.Mips[0].RowPitch == CHECKER_EXTENT * CHANNEL_COUNT);
        CHECK(chain.Mips[1].Offset == CHECKER_BYTES);
        CHECK(chain.Mips[1].RowPitch == CHANNEL_COUNT);
        const uint8 expected = EncodeSrgbCode(0.5F);
        CHECK(expected > 128);
        CHECK(chain.Bytes[CHECKER_BYTES] == expected);
        CHECK(chain.Bytes[CHECKER_BYTES + 1] == expected);
        CHECK(chain.Bytes[CHECKER_BYTES + 2] == expected);
        CHECK(chain.Bytes[CHECKER_BYTES + 3] == CODE_MAXIMUM);
    }
    SECTION("linear data")
    {
        source.Format = rhi::RasterFormat::Rgba8Unorm;
        renderer::Rgba8MipChain chain;
        REQUIRE(renderer::BuildRgba8MipChain(source, 2, chain) == rhi::RasterStatus::Ready);
        CHECK(chain.Bytes[CHECKER_BYTES] == 128);
        CHECK(chain.Bytes[CHECKER_BYTES + 3] == CODE_MAXIMUM);
    }
    SECTION("level zero drops row padding and a single level copies only")
    {
        constexpr usize PADDED_PITCH = CHECKER_EXTENT * CHANNEL_COUNT + CHANNEL_COUNT;
        StaticArray<uint8, PADDED_PITCH * CHECKER_EXTENT> padded{};
        for (usize row = 0; row < CHECKER_EXTENT; ++row)
        {
            for (usize byte = 0; byte < CHECKER_EXTENT * CHANNEL_COUNT; ++byte)
            {
                padded[row * PADDED_PITCH + byte] = checker[row * CHECKER_EXTENT * CHANNEL_COUNT + byte];
            }
        }
        source.Bytes = padded.GetData();
        source.Size = padded.GetSize();
        source.RowPitch = PADDED_PITCH;
        source.Format = rhi::RasterFormat::Rgba8Srgb;
        renderer::Rgba8MipChain chain;
        REQUIRE(renderer::BuildRgba8MipChain(source, 1, chain) == rhi::RasterStatus::Ready);
        REQUIRE(chain.Bytes.GetSize() == CHECKER_BYTES);
        for (usize byte = 0; byte < CHECKER_BYTES; ++byte)
        {
            CHECK(chain.Bytes[byte] == checker[byte]);
        }
    }
}

TEST_CASE("odd extents clamp the second parent texel and invalid chains preserve output", "[texture-mips]")
{
    constexpr usize WIDTH = 3;
    constexpr usize HEIGHT = 1;
    StaticArray<uint8, WIDTH * HEIGHT * CHANNEL_COUNT> row{};
    for (usize texel = 0; texel < WIDTH; ++texel)
    {
        const uint8 code = static_cast<uint8>(texel * 100);
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            row[texel * CHANNEL_COUNT + channel] = code;
        }
    }
    rhi::TextureCpuContent source;
    source.Bytes = row.GetData();
    source.Size = row.GetSize();
    source.RowPitch = WIDTH * CHANNEL_COUNT;
    source.Width = WIDTH;
    source.Height = HEIGHT;
    source.Format = rhi::RasterFormat::Rgba8Unorm;
    renderer::Rgba8MipChain chain;
    REQUIRE(renderer::BuildRgba8MipChain(source, 2, chain) == rhi::RasterStatus::Ready);
    REQUIRE(chain.LevelCount == 2);
    // The one-texel level averages texels 0 and 1 of the only row, twice each: (0 + 100) / 2.
    CHECK(chain.Bytes[WIDTH * CHANNEL_COUNT] == 50);

    renderer::Rgba8MipChain untouched;
    CHECK(renderer::BuildRgba8MipChain(source, 3, untouched) == rhi::RasterStatus::InvalidDescription);
    CHECK(renderer::BuildRgba8MipChain(source, 0, untouched) == rhi::RasterStatus::InvalidDescription);
    source.Format = rhi::RasterFormat::Rgba16Float;
    CHECK(renderer::BuildRgba8MipChain(source, 1, untouched) == rhi::RasterStatus::InvalidDescription);
    source.Format = rhi::RasterFormat::Rgba8Unorm;
    source.RowPitch = CHANNEL_COUNT;
    CHECK(renderer::BuildRgba8MipChain(source, 1, untouched) == rhi::RasterStatus::InvalidDescription);
    CHECK(untouched.LevelCount == 0);
    CHECK(untouched.Bytes.GetSize() == 0);
}
