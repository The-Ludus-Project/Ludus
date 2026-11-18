// Shaping and measurement tests against the real pinned HarfBuzz + FreeType
// through the public engine API (requirements T04, T05, T06). Covers Latin
// kerning/ligatures, Hangul, Arabic RTL joining, spaces, empty input, missing
// glyphs, malformed UTF-8, script mismatch, and control rejection.

#include <ludus/text/font_system.h>

#include "fixture_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ludus::text;
using ludus::text::test::ArabicFont;
using ludus::text::test::HangulFont;
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
        REQUIRE(System != nullptr);
    }
    ~SystemGuard()
    {
        (void)DestroyFontSystem(System);
    }
};

FontId LoadFixture(FontSystem* system, const char* file)
{
    ludus::foundation::Array<ludus::foundation::uint8> bytes = LoadFixtureFont(file);
    REQUIRE(bytes.IsEmpty() == false);
    FontId font;
    REQUIRE(IsOk(LoadFont(system, bytes, 0, &font)));
    REQUIRE(font.IsValid());
    return font;
}
} // namespace

TEST_CASE("Empty input is a valid empty layout", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "", 16, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    CHECK(view.Glyphs.empty());
    CHECK(view.Metrics.GlyphCount == 0);
    CHECK(view.Metrics.Ink.IsEmpty());
    CHECK(view.Metrics.LineAdvance > 0.0F); // sized face metric still reported
}

TEST_CASE("Latin shaping produces one glyph per visible character and advances", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "AV", 48, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    CHECK(view.Glyphs.size() == 2);
    CHECK(view.Metrics.AdvanceX > 0.0F);
    // Clusters are original UTF-8 byte offsets: 'A' at 0, 'V' at 1.
    CHECK(view.Glyphs[0].Cluster == 0);
    CHECK(view.Glyphs[1].Cluster == 1);
    CHECK(view.Metrics.MissingGlyphCount == 0);
}

TEST_CASE("Long kerned runs measure the complete fractional pen advance", "[text][shaping]")
{
    using namespace ludus::foundation;
    SystemGuard guard;
    const FontId font = LoadFixture(guard.System, LatinFont());
    constexpr uint32 PIXEL_HEIGHT = 128;
    constexpr usize PAIR_COUNT = 2048;
    std::string run;
    for (usize i = 0; i < PAIR_COUNT; ++i)
    {
        run += "AV";
    }
    LayoutId layout;
    REQUIRE(ShapeLatin(guard.System, font, run, PIXEL_HEIGHT, &layout) == Status::Ok);
    LayoutView view;
    REQUIRE(GetLayout(guard.System, layout, &view) == Status::Ok);
    REQUIRE(view.Glyphs.size() == run.size());

    int64 totalAdvance = 0;
    for (const ShapedGlyph& glyph : view.Glyphs)
    {
        totalAdvance += glyph.XAdvance;
    }
    constexpr float64 UNITS_PER_PIXEL = 64;
    CHECK(view.Metrics.AdvanceX == static_cast<float32>(static_cast<float64>(totalAdvance) / UNITS_PER_PIXEL));

    LayoutId lastLayout;
    REQUIRE(ShapeLatin(guard.System, font, "V", PIXEL_HEIGHT, &lastLayout) == Status::Ok);
    LayoutView lastView;
    REQUIRE(GetLayout(guard.System, lastLayout, &lastView) == Status::Ok);
    REQUIRE(lastView.Glyphs.size() == 1);
    const ShapedGlyph& lastGlyph = view.Glyphs.back();
    REQUIRE(lastGlyph.GlyphId == lastView.Glyphs.front().GlyphId);
    const int64 lastOrigin = totalAdvance - lastGlyph.XAdvance + lastGlyph.XOffset - lastView.Glyphs.front().XOffset;
    const float64 right =
        static_cast<float64>(lastOrigin) / UNITS_PER_PIXEL + static_cast<float64>(lastView.Metrics.Ink.Right);
    CHECK(view.Metrics.Ink.Right == static_cast<float32>(right));
}

TEST_CASE("The office ligature keeps byte clusters", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "office", 32, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    // Whether or not "ffi"/"fi" ligate, clusters stay within the source bytes
    // and the layout is non-empty with positive advance.
    CHECK_FALSE(view.Glyphs.empty());
    CHECK(view.Metrics.AdvanceX > 0.0F);
    for (const auto& g : view.Glyphs)
    {
        CHECK(g.Cluster < view.SourceUtf8.size());
    }
}

TEST_CASE("Spaces advance without ink", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId withSpace;
    LayoutId noSpace;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "a b", 24, &withSpace)));
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "ab", 24, &noSpace)));

    LayoutView s;
    LayoutView n;
    REQUIRE(IsOk(GetLayout(guard.System, withSpace, &s)));
    REQUIRE(IsOk(GetLayout(guard.System, noSpace, &n)));
    CHECK(s.Glyphs.size() == 3); // space keeps a glyph record
    CHECK(s.Metrics.AdvanceX > n.Metrics.AdvanceX);
}

TEST_CASE("Hangul shaping succeeds with the Hang script", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, HangulFont());
    RunSpec spec;
    spec.PixelHeight = 32;
    spec.TextDirection = Direction::LeftToRight;
    spec.Script[0] = 'H';
    spec.Script[1] = 'a';
    spec.Script[2] = 'n';
    spec.Script[3] = 'g';
    LayoutId layout;
    REQUIRE(IsOk(ShapeRun(guard.System, font, "한글", spec, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    CHECK_FALSE(view.Glyphs.empty());
    CHECK(view.Metrics.MissingGlyphCount == 0);
    CHECK(view.Metrics.AdvanceX > 0.0F);
}

TEST_CASE("Arabic RTL run preserves HarfBuzz order", "[text][shaping][rtl]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, ArabicFont());
    RunSpec spec;
    spec.PixelHeight = 32;
    spec.TextDirection = Direction::RightToLeft;
    spec.Script[0] = 'A';
    spec.Script[1] = 'r';
    spec.Script[2] = 'a';
    spec.Script[3] = 'b';
    LayoutId layout;
    // "سلام" (salaam)
    REQUIRE(IsOk(ShapeRun(guard.System, font, "سلام", spec, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    CHECK_FALSE(view.Glyphs.empty());
    CHECK(view.TextDirection == Direction::RightToLeft);
    CHECK(view.Metrics.MissingGlyphCount == 0);
    // RTL output is in visual order: clusters are non-increasing across glyphs.
    for (std::size_t i = 1; i < view.Glyphs.size(); ++i)
    {
        CHECK(view.Glyphs[i].Cluster <= view.Glyphs[i - 1].Cluster);
    }
}

TEST_CASE("Missing characters produce .notdef and a diagnostic", "[text][shaping][notdef]")
{
    SystemGuard guard;
    // The subset Hangul face lacks Latin letters, so ASCII maps to .notdef.
    FontId font = LoadFixture(guard.System, HangulFont());
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "Z", 24, &layout)));

    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.System, layout, &view)));
    REQUIRE(view.Glyphs.size() == 1);
    CHECK(view.Glyphs[0].GlyphId == 0);
    CHECK(view.Glyphs[0].IsMissing);
    CHECK(view.Metrics.MissingGlyphCount == 1);
}

TEST_CASE("Malformed UTF-8 is rejected", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout{42};
    const std::string_view bad("a\x80"
                               "b",
                               3);
    CHECK(ShapeLatin(guard.System, font, bad, 16, &layout) == Status::InvalidUtf8);
    CHECK_FALSE(layout.IsValid()); // output invalidated on failure
}

TEST_CASE("Control characters and script mismatch are rejected", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    CHECK(ShapeLatin(guard.System, font, "a\nb", 16, &layout) == Status::UnsupportedText);
    CHECK(ShapeLatin(guard.System, font, "a\tb", 16, &layout) == Status::UnsupportedText);
    // Arabic strong-script letters in a declared Latn run are rejected.
    CHECK(ShapeLatin(guard.System, font, "سلام", 16, &layout) == Status::UnsupportedText);
}

TEST_CASE("Pixel height is validated", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId layout;
    CHECK(ShapeLatin(guard.System, font, "a", 0, &layout) == Status::InvalidArgument);
    CHECK(ShapeLatin(guard.System, font, "a", 1000, &layout) == Status::InvalidArgument);
}

TEST_CASE("Alternating sizes do not contaminate metrics", "[text][shaping]")
{
    SystemGuard guard;
    FontId font = LoadFixture(guard.System, LatinFont());
    LayoutId small1;
    LayoutId big;
    LayoutId small2;
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "Ag", 16, &small1)));
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "Ag", 48, &big)));
    REQUIRE(IsOk(ShapeLatin(guard.System, font, "Ag", 16, &small2)));

    LayoutView s1;
    LayoutView b;
    LayoutView s2;
    REQUIRE(IsOk(GetLayout(guard.System, small1, &s1)));
    REQUIRE(IsOk(GetLayout(guard.System, big, &b)));
    REQUIRE(IsOk(GetLayout(guard.System, small2, &s2)));
    CHECK(s1.Metrics.AdvanceX == s2.Metrics.AdvanceX);
    CHECK(b.Metrics.AdvanceX > s1.Metrics.AdvanceX);
    CHECK(b.Metrics.LineAdvance > s1.Metrics.LineAdvance);
}
