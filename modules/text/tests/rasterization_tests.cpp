// Grayscale rasterization tests (requirement T07): coverage range, top-down
// tightly-packed normalization, zero-area glyphs with valid metrics, negative
// bearings, and borrowed-scratch lifetime.

#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/text/font_system.h>

#include "fixture_support.h"
#include "internal/raster_layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace ludus::text;
using ludus::text::test::LatinFont;
using ludus::text::test::LoadFixtureFont;

namespace
{
struct SystemGuard
{
    FontSystem* System = nullptr;
    SystemGuard()
    {
        REQUIRE(IsOk(CreateFontSystem(FontSystemConfig{}, &System)));
    }
    ~SystemGuard()
    {
        (void)DestroyFontSystem(System);
    }
};

FontId LoadFixture(FontSystem* system, const char* file)
{
    const ludus::foundation::Array<ludus::foundation::uint8> bytes = LoadFixtureFont(file);
    REQUIRE(bytes.IsEmpty() == false);
    FontId font;
    REQUIRE(IsOk(LoadFont(system, bytes, 0, &font)));
    return font;
}

// Shape a single character and return its glyph id.
ludus::foundation::uint32 GlyphOf(FontSystem* system, FontId font, const char* ch)
{
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(system, font, ch, 48, &layout)));
    LayoutView view;
    REQUIRE(IsOk(GetLayout(system, layout, &view)));
    REQUIRE(view.Glyphs.size() == 1);
    const auto id = view.Glyphs[0].GlyphId;
    (void)DestroyLayout(system, layout);
    return id;
}
} // namespace

TEST_CASE("A letter rasterizes to tightly packed coverage in range", "[text][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto glyph = GlyphOf(guard.System, font, "A");

    GlyphBitmap bmp;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &bmp)));
    CHECK(bmp.Width > 0);
    CHECK(bmp.Height > 0);
    CHECK(bmp.Coverage.size() == static_cast<std::size_t>(bmp.Width) * bmp.Height);
    CHECK(bmp.BearingTop > 0); // 'A' sits above the baseline

    bool anyInk = false;
    bool anyEmpty = false;
    for (auto v : bmp.Coverage)
    {
        anyInk = anyInk || v > 0;
        anyEmpty = anyEmpty || v == 0;
    }
    CHECK(anyInk);   // actual coverage present
    CHECK(anyEmpty); // corners of 'A' are transparent
}

TEST_CASE("Space is a zero-area glyph with no coverage", "[text][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto glyph = GlyphOf(guard.System, font, " ");

    GlyphBitmap bmp;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &bmp)));
    CHECK(bmp.Width == 0);
    CHECK(bmp.Height == 0);
    CHECK(bmp.Coverage.empty());
}

TEST_CASE("Rasterizing an invalid font handle fails", "[text][raster]")
{
    SystemGuard guard;
    GlyphBitmap bmp;
    CHECK(RasterizeGlyph(guard.System, FontId{}, 48, 1, &bmp) == Status::InvalidHandle);
}

TEST_CASE("Rasterization pixel height is validated", "[text][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    GlyphBitmap bmp;
    CHECK(RasterizeGlyph(guard.System, font, 0, 1, &bmp) == Status::InvalidArgument);
    CHECK(RasterizeGlyph(guard.System, font, 300, 1, &bmp) == Status::InvalidArgument);
}

TEST_CASE("Scratch is only valid until the next raster call", "[text][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto a = GlyphOf(guard.System, font, "A");
    const auto b = GlyphOf(guard.System, font, "B");

    GlyphBitmap first;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, a, &first)));
    // Copy immediately, as the contract requires.
    const ludus::foundation::Array<ludus::foundation::uint8> copy =
        ludus::foundation::Array<ludus::foundation::uint8>::FromRange(
            ludus::foundation::MakeBorrowedView(first.Coverage));

    GlyphBitmap second;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, b, &second)));
    // The copy is still intact and independent of the second raster.
    CHECK(copy.size() == static_cast<std::size_t>(first.Width) * first.Height);
}

TEST_CASE("raster layout checks signed pitch and byte extents before allocation", "[primitive][text][raster]")
{
    using namespace ludus::foundation;
    internal::RasterLayout layout{99, 98};
    REQUIRE(internal::TryRasterLayout({ .Width = 3, .Height = 2, .Pitch = 4 }, layout) == Status::Ok);
    REQUIRE(layout.Pitch == 4);
    REQUIRE(layout.CoverageBytes == 6);
    REQUIRE(internal::TryRasterLayout({ .Width = 3, .Height = 2, .Pitch = -4 }, layout) == Status::Ok);
    REQUIRE(layout.Pitch == 4);
    REQUIRE(layout.CoverageBytes == 6);
    REQUIRE(internal::TryRasterLayout({ .Width = 3, .Height = 2, .Pitch = 2 }, layout) == Status::BackendFailure);
    REQUIRE(layout.Pitch == 4);
    REQUIRE(layout.CoverageBytes == 6);
    constexpr int32 minPitch = -int32{2147483647} - 1;
    REQUIRE(internal::TryRasterLayout({ .Width = 1, .Height = 1, .Pitch = minPitch }, layout) == Status::Ok);
    REQUIRE(layout.Pitch == uint64{2147483648});
    REQUIRE(layout.CoverageBytes == 1);
    const auto extremeStatus = internal::TryRasterLayout(
        {
            .Width = 1,
            .Height = ~uint32{0},
            .Pitch = minPitch,
        },
        layout);
    if constexpr (sizeof(usize) == 4)
    {
        REQUIRE(extremeStatus == Status::ResourceLimit);
        REQUIRE(layout.CoverageBytes == 1);
    }
    else
    {
        REQUIRE(extremeStatus == Status::Ok);
        REQUIRE(layout.CoverageBytes == ~uint32{0});
    }
    REQUIRE(internal::TryRasterLayout({ .Width = 0, .Height = ~uint32{0}, .Pitch = minPitch }, layout) == Status::Ok);
    REQUIRE(layout.Pitch == 0);
    REQUIRE(layout.CoverageBytes == 0);
}
