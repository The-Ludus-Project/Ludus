#include "fixture_support.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/text/coverage_atlas.hpp>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/graphics/ui_text/label_cache.hpp>
#include <ludus/graphics/ui_text/ui_text.hpp>
#include <ludus/text/font_system.h>
#include <ludus/ui/context.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <span>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace cpu = ludus::text;
namespace ui = ludus::ui;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace gt = ludus::graphics::text;
namespace ui_render = ludus::graphics::ui_render;
namespace ui_text = ludus::graphics::ui_text;
using Status = rhi::RasterStatus;

constexpr uint32 LABEL_PIXELS = 16;
constexpr uint64 LATIN_VERSION = 1;

/// One FontSystem with the packaged fixture fonts, destroyed after its layouts.
class Fonts final
{
public:
    Fonts()
    {
        REQUIRE(cpu::CreateFontSystem({}, &mSystemOrNull) == cpu::Status::Ok);
    }
    ~Fonts()
    {
        for (usize index = 0; index < mLayoutCount; ++index)
        {
            static_cast<void>(cpu::DestroyLayout(mSystemOrNull, mLayouts[index]));
        }
        static_cast<void>(cpu::DestroyFontSystem(mSystemOrNull));
    }
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;
    Fonts(Fonts&&) = delete;
    Fonts& operator=(Fonts&&) = delete;

    [[nodiscard]] cpu::FontSystem* GetSystem() const noexcept
    {
        return mSystemOrNull;
    }

    [[nodiscard]] cpu::FontId Load(const char* fileName)
    {
        const Array<uint8> bytes = cpu::test::LoadFixtureFont(fileName);
        REQUIRE(bytes.IsEmpty() == false);
        cpu::FontId font;
        REQUIRE(cpu::LoadFont(mSystemOrNull, bytes, 0, &font) == cpu::Status::Ok);
        return font;
    }

    [[nodiscard]] cpu::LayoutId Shape(cpu::FontId font,
                                      std::string_view utf8,
                                      uint32 pixelHeight,
                                      std::string_view script = "Latn",
                                      cpu::Direction direction = cpu::Direction::LeftToRight)
    {
        cpu::RunSpec spec;
        spec.PixelHeight = pixelHeight;
        spec.TextDirection = direction;
        for (usize index = 0; index < script.size(); ++index)
        {
            spec.Script[index] = script[index];
        }
        cpu::LayoutId layout;
        REQUIRE(cpu::ShapeRun(mSystemOrNull, font, utf8, spec, &layout) == cpu::Status::Ok);
        REQUIRE(mLayoutCount < MAX_LAYOUTS);
        mLayouts[mLayoutCount++] = layout;
        return layout;
    }

    [[nodiscard]] usize GetGlyphCount(cpu::LayoutId layout) const
    {
        cpu::LayoutView view;
        REQUIRE(cpu::GetLayout(mSystemOrNull, layout, &view) == cpu::Status::Ok);
        return view.Glyphs.size();
    }

private:
    static constexpr usize MAX_LAYOUTS = 128;
    cpu::FontSystem* mSystemOrNull = nullptr;
    core::StaticArray<cpu::LayoutId, MAX_LAYOUTS> mLayouts;
    usize mLayoutCount = 0;
};

[[nodiscard]] ui_text::LabelSource MakeSource(ui::WidgetId element, cpu::LayoutId layout) noexcept
{
    ui_text::LabelSource source;
    source.Element = element;
    source.Layout = layout;
    source.FontVersion = LATIN_VERSION;
    source.Scale = 1;
    source.OffsetX = 2;
    source.BaselineY = 12;
    return source;
}

[[nodiscard]] ui_text::LabelSource WithVersion(ui_text::LabelSource source, uint64 fontVersion) noexcept
{
    source.FontVersion = fontVersion;
    return source;
}

[[nodiscard]] ui_text::LabelSource WithScale(ui_text::LabelSource source, float32 scale) noexcept
{
    source.Scale = scale;
    return source;
}

[[nodiscard]] const ui_text::Label* FindLabel(const ui_text::LabelCache& cache, ui::WidgetId element) noexcept
{
    for (const ui_text::Label& label : cache.GetLabels())
    {
        if (label.Element == element)
        {
            return &label;
        }
    }
    return nullptr;
}

/// Sum of the atlas coverage a label's glyphs address, to compare labels across atlas versions.
[[nodiscard]] uint64 SumInk(const gt::CoverageAtlas& atlas, const ui_text::Label& label) noexcept
{
    uint64 sum = 0;
    const float32 extent = static_cast<float32>(gt::CoverageAtlas::EXTENT);
    for (const gt::TextQuad& glyph : label.Glyphs)
    {
        const usize left = static_cast<usize>(glyph.Uv.X * extent);
        const usize top = static_cast<usize>(glyph.Uv.Y * extent);
        const usize right = static_cast<usize>(glyph.Uv.Z * extent);
        const usize bottom = static_cast<usize>(glyph.Uv.W * extent);
        for (usize y = top; y < bottom; ++y)
        {
            for (usize x = left; x < right; ++x)
            {
                sum += atlas.Coverage()[y * gt::CoverageAtlas::EXTENT + x];
            }
        }
    }
    return sum;
}

/// A one-element document whose element the labels name.
constexpr ui::WidgetId LABELLED = 7;
constexpr float32 VIEWPORT_EXTENT = 128;

[[nodiscard]] ui::Element MakeElement(ui::WidgetId id) noexcept
{
    ui::Element element;
    element.Id = id;
    element.Placement.Width = {VIEWPORT_EXTENT, ui::Unit::Pixels};
    element.Placement.Height = {VIEWPORT_EXTENT, ui::Unit::Pixels};
    return element;
}
} // namespace

TEST_CASE("LabelCache copies Latin, Hangul and Arabic shaping fixtures and reuses warm glyphs", "[ui_text][text]")
{
    Fonts fonts;
    ui_text::LabelCache cache;
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    CHECK(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    const core::StaticArray<const char*, 3> files{
        {cpu::test::LatinFont(), cpu::test::HangulFont(), cpu::test::ArabicFont()}};
    const core::StaticArray<std::string_view, 3> texts{{"AV office", "한글", "مرحبا"}};
    const core::StaticArray<std::string_view, 3> scripts{{"Latn", "Hang", "Arab"}};
    core::StaticArray<cpu::LayoutId, 3> layouts;
    for (usize run = 0; run < 3; ++run)
    {
        const cpu::FontId font = fonts.Load(files[run]);
        const cpu::Direction direction = run == 2 ? cpu::Direction::RightToLeft : cpu::Direction::LeftToRight;
        layouts[run] = fonts.Shape(font, texts[run], LABEL_PIXELS, scripts[run], direction);
        ui_text::LabelUpdate update;
        REQUIRE(cache.SetLabel(WithVersion(MakeSource(run + 1, layouts[run]), run + 1), update) == Status::Ready);
        CHECK(update.HasCompacted == false);
        CHECK(update.Repair == ui_text::LabelRepair::None);
        CHECK(update.Labels == run + 1);
        CHECK(update.RequestedGlyphs == fonts.GetGlyphCount(layouts[run]));
        const ui_text::Label* label = FindLabel(cache, run + 1);
        REQUIRE(label != nullptr);
        REQUIRE(label->Glyphs.size() == fonts.GetGlyphCount(layouts[run]));
        cpu::LayoutView view;
        REQUIRE(cpu::GetLayout(fonts.GetSystem(), layouts[run], &view) == cpu::Status::Ok);
        for (usize glyph = 0; glyph < view.Glyphs.size(); ++glyph)
        {
            CHECK(label->Glyphs[glyph].Cluster == view.Glyphs[glyph].Cluster);
        }
        CHECK(SumInk(cache.GetAtlas(), *label) > 0);
    }
    // Arabic is shaped right to left: clusters run backwards through the source bytes.
    const ui_text::Label* arabic = FindLabel(cache, 3);
    REQUIRE(arabic != nullptr);
    CHECK(arabic->Glyphs.front().Cluster > arabic->Glyphs.back().Cluster);
    // Setting the same runs again hits the cache: no rasterization and no new revision.
    const uint64 rasterized = cpu::GetCounters(fonts.GetSystem()).RasterizeCalls;
    const uint64 revision = cache.GetAtlasRevision();
    for (usize run = 0; run < 3; ++run)
    {
        ui_text::LabelUpdate update;
        REQUIRE(cache.SetLabel(WithVersion(MakeSource(run + 1, layouts[run]), run + 1), update) == Status::Ready);
    }
    CHECK(cpu::GetCounters(fonts.GetSystem()).RasterizeCalls == rasterized);
    CHECK(cache.GetAtlasRevision() == revision);
    CHECK(cache.GetLabels().size() == 3);
}

TEST_CASE("LabelCache keys glyphs by raster size across display-scale changes", "[ui_text][text]")
{
    Fonts fonts;
    const cpu::FontId font = fonts.Load(cpu::test::LatinFont());
    ui_text::LabelCache cache;
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    ui::Context context;
    REQUIRE(context.TryInitialize(1) == ui::Status::Ok);
    const ui::Element element = MakeElement(LABELLED);
    REQUIRE(context.TrySetDocument({&element, 1}, {{0, 0, VIEWPORT_EXTENT, VIEWPORT_EXTENT}}) == ui::Status::Ok);
    renderer::OverlayList list;
    REQUIRE(list.Initialize() == Status::Ready);
    const auto draw = [&](float32 scale) {
        list.Clear();
        const uint32 extent = static_cast<uint32>(VIEWPORT_EXTENT * scale);
        ui_text::UiTextReport report;
        return ui_text::AppendUi(list,
                                 context.GetLayout(),
                                 context.GetPaintCommands(),
                                 cache.GetLabels(),
                                 {scale, 0, 0, extent, extent},
                                 report);
    };

    ui_text::LabelUpdate update;
    REQUIRE(cache.SetLabel(MakeSource(LABELLED, fonts.Shape(font, "Ludus", LABEL_PIXELS)), update) == Status::Ready);
    const usize oneTimes = cache.GetAtlas().GlyphCount();
    const uint64 oneTimesRevision = cache.GetAtlasRevision();
    REQUIRE(draw(1) == Status::Ready);
    // At 2x the run is shaped at twice the pixel height; none of the 1x cells can serve it.
    const uint64 beforeDouble = cpu::GetCounters(fonts.GetSystem()).RasterizeCalls;
    REQUIRE(cache.SetLabel(WithScale(MakeSource(LABELLED, fonts.Shape(font, "Ludus", 2 * LABEL_PIXELS)), 2), update) ==
            Status::Ready);
    CHECK(cache.GetAtlas().GlyphCount() == 2 * oneTimes);
    CHECK(cpu::GetCounters(fonts.GetSystem()).RasterizeCalls - beforeDouble == oneTimes);
    CHECK(cache.GetAtlasRevision() > oneTimesRevision);
    // A label rasterized for 2x cannot draw at 1x.
    CHECK(draw(1) == Status::InvalidDescription);
    CHECK(draw(2) == Status::Ready);
    // Returning to 1x reuses the 1x cells without rasterizing.
    const uint64 beforeReturn = cpu::GetCounters(fonts.GetSystem()).RasterizeCalls;
    const uint64 doubleRevision = cache.GetAtlasRevision();
    REQUIRE(cache.SetLabel(MakeSource(LABELLED, fonts.Shape(font, "Ludus", LABEL_PIXELS)), update) == Status::Ready);
    CHECK(cpu::GetCounters(fonts.GetSystem()).RasterizeCalls == beforeReturn);
    CHECK(cache.GetAtlas().GlyphCount() == 2 * oneTimes);
    CHECK(cache.GetAtlasRevision() == doubleRevision);
    CHECK(draw(1) == Status::Ready);
}

TEST_CASE("LabelCache compacts stale glyphs automatically when the atlas is full", "[ui_text][text]")
{
    Fonts fonts;
    const cpu::FontId font = fonts.Load(cpu::test::LatinFont());
    ui_text::LabelCache cache;
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    ui_text::LabelUpdate update;
    constexpr ui::WidgetId KEPT = 1;
    REQUIRE(cache.SetLabel(MakeSource(KEPT, fonts.Shape(font, "kept", LABEL_PIXELS)), update) == Status::Ready);
    const ui_text::Label* kept = FindLabel(cache, KEPT);
    REQUIRE(kept != nullptr);
    const uint64 keptInk = SumInk(cache.GetAtlas(), *kept);
    // Replace another element's label with ever larger single glyphs. Each size is a new cell, so
    // stale cells fill the atlas until a replacement needs compaction.
    constexpr uint32 FIRST_SIZE = 96;
    constexpr uint32 LAST_SIZE = 160;
    bool hasCompacted = false;
    for (uint32 size = FIRST_SIZE; size <= LAST_SIZE && hasCompacted == false; ++size)
    {
        const uint64 revision = cache.GetAtlasRevision();
        REQUIRE(cache.SetLabel(MakeSource(LABELLED, fonts.Shape(font, "W", size)), update) == Status::Ready);
        hasCompacted = update.HasCompacted;
        CHECK(cache.GetAtlasRevision() > revision);
    }
    REQUIRE(hasCompacted == true);
    // After compaction only the live labels' cells remain: "kept" plus one "W".
    CHECK(update.Repair == ui_text::LabelRepair::None);
    CHECK(update.AtlasGlyphs == cache.GetAtlas().GlyphCount());
    kept = FindLabel(cache, KEPT);
    REQUIRE(kept != nullptr);
    CHECK(cache.GetAtlas().GlyphCount() == kept->Glyphs.size() + 1);
    // The kept label moved with its exact coverage.
    CHECK(SumInk(cache.GetAtlas(), *kept) == keptInk);
}

TEST_CASE("LabelCache keeps the last ready label when compaction cannot make room", "[ui_text][text]")
{
    Fonts fonts;
    const cpu::FontId font = fonts.Load(cpu::test::LatinFont());
    ui_text::LabelCache cache;
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    ui_text::LabelUpdate update;
    REQUIRE(cache.SetLabel(MakeSource(LABELLED, fonts.Shape(font, "ready", LABEL_PIXELS)), update) == Status::Ready);
    const ui_text::Label before = *FindLabel(cache, LABELLED);
    const core::StaticArray<gt::TextQuad, 5> glyphs{
        {before.Glyphs[0], before.Glyphs[1], before.Glyphs[2], before.Glyphs[3], before.Glyphs[4]}};
    const uint64 ink = SumInk(cache.GetAtlas(), before);
    const uint64 revision = cache.GetAtlasRevision();
    // Two glyphs at the largest raster size cannot share one 256 x 256 page.
    constexpr uint32 HUGE_SIZE = 256;
    const cpu::LayoutId huge = fonts.Shape(font, "MW", HUGE_SIZE);
    update = {};
    CHECK(cache.SetLabel(MakeSource(LABELLED, huge), update) == Status::CapacityExceeded);
    CHECK(update.Repair == ui_text::LabelRepair::ReduceGlyphs);
    CHECK(update.HasCompacted == false);
    CHECK(update.RequestedGlyphs == 2);
    CHECK(update.Labels == 1);
    // The label being replaced keeps no cells in the compaction attempt, which placed only "M"
    // before "W" ran out of rows.
    CHECK(update.AtlasGlyphs == 1);
    CHECK(update.OccupiedTexels > 0);
    // The element's previous label, its UVs and their coverage are unchanged.
    const ui_text::Label* after = FindLabel(cache, LABELLED);
    REQUIRE(after != nullptr);
    REQUIRE(after->Glyphs.size() == glyphs.GetSize());
    for (usize index = 0; index < glyphs.GetSize(); ++index)
    {
        CHECK(after->Glyphs[index].Uv.X == glyphs[index].Uv.X);
        CHECK(after->Glyphs[index].Uv.W == glyphs[index].Uv.W);
        CHECK(after->Glyphs[index].Rectangle.X == glyphs[index].Rectangle.X);
    }
    CHECK(SumInk(cache.GetAtlas(), *after) == ink);
    CHECK(cache.GetAtlasRevision() == revision);
    // The repair works: a smaller size fits.
    REQUIRE(cache.SetLabel(MakeSource(LABELLED, fonts.Shape(font, "MW", LABEL_PIXELS)), update) == Status::Ready);
}

TEST_CASE("LabelCache reports slot and label-length limits with their repairs", "[ui_text][text]")
{
    Fonts fonts;
    const cpu::FontId font = fonts.Load(cpu::test::LatinFont());
    ui_text::LabelCache cache;
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    const cpu::LayoutId layout = fonts.Shape(font, "slot", LABEL_PIXELS);
    ui_text::LabelUpdate update;
    for (usize element = 1; element <= ui_text::LabelCache::MAX_LABELS; ++element)
    {
        REQUIRE(cache.SetLabel(MakeSource(element, layout), update) == Status::Ready);
    }
    CHECK(cache.SetLabel(MakeSource(ui_text::LabelCache::MAX_LABELS + 1, layout), update) == Status::CapacityExceeded);
    CHECK(update.Repair == ui_text::LabelRepair::RemoveLabel);
    CHECK(update.Labels == ui_text::LabelCache::MAX_LABELS);
    // Replacing an existing element's label still works when every slot is used.
    CHECK(cache.SetLabel(MakeSource(1, layout), update) == Status::Ready);
    CHECK(cache.RemoveLabel(1) == true);
    CHECK(cache.RemoveLabel(1) == false);
    CHECK(cache.GetLabels().size() == ui_text::LabelCache::MAX_LABELS - 1);
    CHECK(FindLabel(cache, ui_text::LabelCache::MAX_LABELS) != nullptr);
    CHECK(cache.SetLabel(MakeSource(ui_text::LabelCache::MAX_LABELS + 1, layout), update) == Status::Ready);
    // A run longer than one label is rejected before touching the atlas.
    const cpu::LayoutId longRun =
        fonts.Shape(font, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", LABEL_PIXELS);
    REQUIRE(fonts.GetGlyphCount(longRun) > ui_text::LabelCache::MAX_LABEL_GLYPHS);
    const usize cells = cache.GetAtlas().GlyphCount();
    CHECK(cache.SetLabel(MakeSource(2, longRun), update) == Status::CapacityExceeded);
    CHECK(update.Repair == ui_text::LabelRepair::ShortenLabel);
    CHECK(update.RequestedGlyphs == fonts.GetGlyphCount(longRun));
    CHECK(cache.GetAtlas().GlyphCount() == cells);
}

TEST_CASE("LabelCache rejects invalid sources and stale layouts without changing labels", "[ui_text][text]")
{
    Fonts fonts;
    const cpu::FontId font = fonts.Load(cpu::test::LatinFont());
    ui_text::LabelCache cache;
    ui_text::LabelUpdate update;
    update.Labels = 99;
    const cpu::LayoutId layout = fonts.Shape(font, "ok", LABEL_PIXELS);
    CHECK(cache.SetLabel(MakeSource(LABELLED, layout), update) == Status::NotReady);
    CHECK(cache.GetLabels().empty() == true);
    CHECK(cache.Initialize(nullptr) == Status::InvalidDescription);
    REQUIRE(cache.Initialize(fonts.GetSystem()) == Status::Ready);
    CHECK(cache.SetLabel(MakeSource(0, layout), update) == Status::InvalidDescription);
    CHECK(cache.SetLabel(WithVersion(MakeSource(LABELLED, layout), 0), update) == Status::InvalidDescription);
    CHECK(cache.SetLabel(WithScale(MakeSource(LABELLED, layout), std::numeric_limits<float32>::infinity()), update) ==
          Status::InvalidDescription);
    ui_text::LabelSource colored = MakeSource(LABELLED, layout);
    colored.Color.A = 2;
    CHECK(cache.SetLabel(colored, update) == Status::InvalidDescription);
    cpu::LayoutId stale;
    REQUIRE(cpu::ShapeLatin(fonts.GetSystem(), font, "gone", LABEL_PIXELS, &stale) == cpu::Status::Ok);
    REQUIRE(cpu::DestroyLayout(fonts.GetSystem(), stale) == cpu::Status::Ok);
    CHECK(cache.SetLabel(MakeSource(LABELLED, stale), update) == Status::InvalidHandle);
    CHECK(update.Labels == 99);
    CHECK(cache.GetLabels().empty() == true);
}
