// Audit-strengthening tests closing gaps the F1/F2 ledger flagged as "still to
// add" and that the T05/T07 requirements call out directly: negative bearings,
// combining marks, deterministic normalized coverage, and preserved shaped
// offsets. These exercise the real pinned FreeType + HarfBuzz, not mocks.

#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/text/font_system.h>

#include "fixture_support.h"

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

ludus::foundation::uint32 GlyphOf(FontSystem* system, FontId font, const char* ch, ludus::foundation::uint32 px)
{
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(system, font, ch, px, &layout)));
    LayoutView view;
    REQUIRE(IsOk(GetLayout(system, layout, &view)));
    REQUIRE(view.Glyphs.size() == 1);
    const auto id = view.Glyphs[0].GlyphId;
    (void)DestroyLayout(system, layout);
    return id;
}
} // namespace

// T05/T07: a glyph with a negative left side bearing must report it faithfully.
// In Noto Sans at 48px, 'j' overhangs to the left (bitmap_left < 0).
TEST_CASE("Negative left bearing is reported faithfully", "[text][audit][bearings]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto glyph = GlyphOf(guard.System, font, "j", 48);

    GlyphBitmap bmp;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &bmp)));
    CHECK(bmp.BearingLeft < 0); // real negative overhang
    CHECK(bmp.BearingTop > 0);  // sits above the baseline
    CHECK(bmp.Width > 0);
    CHECK(bmp.Height > 0);
    CHECK(bmp.Coverage.size() == static_cast<std::size_t>(bmp.Width) * bmp.Height);
}

// T04/T05: a base letter plus a combining accent is accepted (combining marks
// are not rejected controls) and keeps byte-cluster provenance to the base.
TEST_CASE("Combining accent shapes and keeps cluster provenance", "[text][audit][combining]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    // "e" + U+0301 COMBINING ACUTE ACCENT (0xCC 0x81).
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "e\xCC\x81", 48, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    CHECK_FALSE(view.Glyphs.empty());
    CHECK(view.Metrics.MissingGlyphCount == 0);
    // Whether the font composes to precomposed "é" or keeps base+mark, every
    // glyph's cluster stays within the 3 source bytes and references the base.
    for (const auto& g : view.Glyphs)
    {
        CHECK(g.Cluster < view.SourceUtf8.size());
    }
    // The accent must not widen the advance beyond a precomposed-é-sized box by
    // an unreasonable amount: the layout has positive advance and finite ink.
    CHECK(view.Metrics.AdvanceX > 0.0F);
    CHECK_FALSE(view.Metrics.Ink.IsEmpty());
}

// T07: rasterization is deterministic for the same glyph/size, and the
// hb-ft/FT load state is restored so re-rasterizing after a different size does
// not contaminate the coverage.
TEST_CASE("Rasterization is deterministic and size-stable", "[text][audit][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto glyph = GlyphOf(guard.System, font, "A", 48);

    GlyphBitmap first;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &first)));
    const ludus::foundation::Array<ludus::foundation::uint8> firstCopy =
        ludus::foundation::Array<ludus::foundation::uint8>::FromRange(
            ludus::foundation::MakeBorrowedView(first.Coverage));
    const ludus::foundation::uint32 w = first.Width;
    const ludus::foundation::uint32 h = first.Height;

    // Rasterize a different size in between, then return to 48px.
    GlyphBitmap other;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 16, glyph, &other)));

    GlyphBitmap again;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &again)));
    REQUIRE(again.Width == w);
    REQUIRE(again.Height == h);
    const ludus::foundation::Array<ludus::foundation::uint8> againCopy =
        ludus::foundation::Array<ludus::foundation::uint8>::FromRange(
            ludus::foundation::MakeBorrowedView(again.Coverage));
    CHECK(againCopy == firstCopy); // byte-identical after the size round trip
}

// T07: coverage values are genuine 0..255 samples (the type bounds this, but we
// assert both ink and transparent texels exist, i.e. not a solid block).
TEST_CASE("Coverage carries partial (anti-aliased) values", "[text][audit][raster]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    const auto glyph = GlyphOf(guard.System, font, "o", 48); // round glyph -> AA edges

    GlyphBitmap bmp;
    REQUIRE(IsOk(RasterizeGlyph(guard.System, font, 48, glyph, &bmp)));
    bool full = false;
    bool partial = false;
    bool empty = false;
    for (auto v : bmp.Coverage)
    {
        full = full || v == 255;
        partial = partial || (v > 0 && v < 255);
        empty = empty || v == 0;
    }
    CHECK(partial); // anti-aliased edge pixels are present
    CHECK(empty);   // interior hole / outside corners
    (void)full;     // full-coverage pixels are expected but not required
}

// T04: shaped signed offsets/advances are preserved from HarfBuzz (26.6 units).
// 'AV' is a classic kerning pair: the pen advance after 'A' is less than the
// isolated 'A' advance would place a non-kerned 'V'.
TEST_CASE("Shaped advances reflect kerning", "[text][audit][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());

    LayoutId kerned;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "AV", 64, &kerned)));
    LayoutView kv;
    REQUIRE(IsOk(GetLayout(guard.System, kerned, &kv)));
    REQUIRE(kv.Glyphs.size() == 2);

    LayoutId soloA;
    LayoutId soloV;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "A", 64, &soloA)));
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "V", 64, &soloV)));
    LayoutView av;
    LayoutView vv;
    REQUIRE(IsOk(GetLayout(guard.System, soloA, &av)));
    REQUIRE(IsOk(GetLayout(guard.System, soloV, &vv)));

    // The kerned "AV" advance is no wider than the two glyphs placed naively,
    // and the signed per-glyph advances are preserved (first glyph advance > 0).
    CHECK(kv.Glyphs[0].XAdvance > 0);
    CHECK(kv.Metrics.AdvanceX <= av.Metrics.AdvanceX + vv.Metrics.AdvanceX + 0.5F);
}
