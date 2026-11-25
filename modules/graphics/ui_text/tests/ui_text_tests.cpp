#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/text/coverage_atlas.hpp>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/graphics/ui_text/ui_text.hpp>
#include <ludus/ui/context.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <span>

namespace
{
using namespace ludus::foundation;
namespace ui = ludus::ui;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace gt = ludus::graphics::text;
namespace ui_render = ludus::graphics::ui_render;
namespace ui_text = ludus::graphics::ui_text;
using Status = rhi::RasterStatus;

constexpr usize QUAD_VERTEX_COUNT = 6;
/// Clip-space vertices are float32 NDC; their pixel reconstruction is exact to well below a pixel.
constexpr float32 PIXEL_TOLERANCE = 1e-3F;
constexpr ui::Color RED{1, 0, 0, 1};
constexpr ui::Color BLUE{0, 0, 1, 1};
constexpr ui::Color GRAY{0.25F, 0.25F, 0.25F, 1};
constexpr ui::Color WHITE{1, 1, 1, 1};

// A clipped panel holds button A and a transparent text element T; sibling button B is painted
// after both and overlaps them.
constexpr ui::WidgetId PANEL = 1;
constexpr ui::WidgetId BUTTON = 2;
constexpr ui::WidgetId CAPTION = 3;
constexpr ui::WidgetId SIBLING = 4;
constexpr ui::WidgetId HIDDEN = 5;
constexpr ui::WidgetId MISSING = 99;
constexpr usize ELEMENT_COUNT = 5;
using Document = core::StaticArray<ui::Element, ELEMENT_COUNT>;
constexpr float32 VIEWPORT_WIDTH = 64;
constexpr float32 VIEWPORT_HEIGHT = 48;
constexpr float32 PANEL_OFFSET = 4;
constexpr float32 PANEL_WIDTH = 30;
constexpr float32 PANEL_HEIGHT = 24;
constexpr float32 BUTTON_OFFSET = 2;
constexpr float32 BUTTON_WIDTH = 20;
constexpr float32 BUTTON_HEIGHT = 10;
constexpr float32 CAPTION_OFFSET_Y = 14;
constexpr float32 CAPTION_WIDTH = 26;
constexpr float32 CAPTION_HEIGHT = 8;
constexpr float32 SIBLING_OFFSET_X = 16;
constexpr float32 SIBLING_OFFSET_Y = 8;
constexpr float32 SIBLING_WIDTH = 8;
constexpr float32 SIBLING_HEIGHT = 30;
constexpr float32 HIDDEN_EXTENT = 6;

[[nodiscard]] Document MakeDocument() noexcept
{
    Document document;
    ui::Element& panel = document[0];
    panel.Id = PANEL;
    panel.Placement.Width = {PANEL_WIDTH, ui::Unit::Pixels};
    panel.Placement.Height = {PANEL_HEIGHT, ui::Unit::Pixels};
    panel.Placement.OffsetX = PANEL_OFFSET;
    panel.Placement.OffsetY = PANEL_OFFSET;
    panel.Fill = GRAY;
    ui::Element& button = document[1];
    button.Id = BUTTON;
    button.Parent = 0;
    button.SemanticRole = ui::Role::Button;
    button.Placement.Width = {BUTTON_WIDTH, ui::Unit::Pixels};
    button.Placement.Height = {BUTTON_HEIGHT, ui::Unit::Pixels};
    button.Placement.OffsetX = BUTTON_OFFSET;
    button.Placement.OffsetY = BUTTON_OFFSET;
    button.Fill = RED;
    ui::Element& caption = document[2];
    caption.Id = CAPTION;
    caption.Parent = 0;
    caption.Placement.Width = {CAPTION_WIDTH, ui::Unit::Pixels};
    caption.Placement.Height = {CAPTION_HEIGHT, ui::Unit::Pixels};
    caption.Placement.OffsetX = BUTTON_OFFSET;
    caption.Placement.OffsetY = CAPTION_OFFSET_Y;
    // Transparent: Ui paints nothing for it, but its label still draws.
    ui::Element& sibling = document[3];
    sibling.Id = SIBLING;
    sibling.SemanticRole = ui::Role::Button;
    sibling.Placement.Width = {SIBLING_WIDTH, ui::Unit::Pixels};
    sibling.Placement.Height = {SIBLING_HEIGHT, ui::Unit::Pixels};
    sibling.Placement.OffsetX = SIBLING_OFFSET_X;
    sibling.Placement.OffsetY = SIBLING_OFFSET_Y;
    sibling.Fill = BLUE;
    ui::Element& hidden = document[4];
    hidden.Id = HIDDEN;
    hidden.Placement.Width = {HIDDEN_EXTENT, ui::Unit::Pixels};
    hidden.Placement.Height = {HIDDEN_EXTENT, ui::Unit::Pixels};
    hidden.Fill = RED;
    hidden.Visible = false;
    return document;
}

struct Fixture final
{
    ui::Context Context;
    renderer::OverlayList List;

    Fixture()
    {
        REQUIRE(Context.TryInitialize(ELEMENT_COUNT) == ui::Status::Ok);
        const Document document = MakeDocument();
        REQUIRE(Context.TrySetDocument({document.GetData(), document.GetSize()},
                                       {{0, 0, VIEWPORT_WIDTH, VIEWPORT_HEIGHT}}) == ui::Status::Ok);
        REQUIRE(List.Initialize() == Status::Ready);
    }

    [[nodiscard]] Status Append(std::span<const ui_text::Label> labels,
                                const ui_render::SurfaceMapping& mapping,
                                ui_text::UiTextReport& outReport)
    {
        return ui_text::AppendUi(List, Context.GetLayout(), Context.GetPaintCommands(), labels, mapping, outReport);
    }
};

[[nodiscard]] ui_render::SurfaceMapping GetMapping(float32 scale) noexcept
{
    return {scale, 0, 0, static_cast<uint32>(VIEWPORT_WIDTH * scale), static_cast<uint32>(VIEWPORT_HEIGHT * scale)};
}

/// Three 4 x 6 baseline-relative glyphs with a one-pixel advance gap, addressing distinct UV cells.
constexpr usize GLYPH_COUNT = 3;
constexpr float32 GLYPH_WIDTH = 4;
constexpr float32 GLYPH_ASCENT = 6;
constexpr float32 GLYPH_PITCH = 5;
using Glyphs = core::StaticArray<gt::TextQuad, GLYPH_COUNT>;

[[nodiscard]] Glyphs MakeGlyphs() noexcept
{
    Glyphs glyphs;
    constexpr float32 UV_STEP = 0.125F;
    for (usize index = 0; index < GLYPH_COUNT; ++index)
    {
        const float32 left = GLYPH_PITCH * static_cast<float32>(index);
        const float32 uvLeft = UV_STEP * static_cast<float32>(index);
        glyphs[index] = {{left, -GLYPH_ASCENT, left + GLYPH_WIDTH, 0},
                         {uvLeft, 0, uvLeft + UV_STEP, UV_STEP},
                         static_cast<uint32>(index)};
    }
    return glyphs;
}

[[nodiscard]] ui_text::Label MakeLabel(ui::WidgetId element, std::span<const gt::TextQuad> glyphs) noexcept
{
    ui_text::Label label;
    label.Element = element;
    label.Glyphs = glyphs;
    label.OffsetX = 1;
    label.BaselineY = GLYPH_ASCENT + 1;
    label.Color = WHITE;
    label.Scale = 1;
    return label;
}

[[nodiscard]] bool IsNear(float32 value, float32 expected) noexcept
{
    return value > expected - PIXEL_TOLERANCE && value < expected + PIXEL_TOLERANCE;
}

/// Physical left and top edges of one appended quad, reconstructed from its first vertex.
[[nodiscard]] bool IsQuadAt(const renderer::OverlayVertex* vertices,
                            usize quad,
                            const ui_render::SurfaceMapping& mapping,
                            float32 left,
                            float32 top) noexcept
{
    const renderer::OverlayVertex& corner = vertices[quad * QUAD_VERTEX_COUNT];
    const float32 x = (corner.Clip.X + 1) * 0.5F * static_cast<float32>(mapping.Width);
    const float32 y = (1 - corner.Clip.Y) * 0.5F * static_cast<float32>(mapping.Height);
    return IsNear(x, left) == true && IsNear(y, top) == true;
}

[[nodiscard]] bool IsCoverageQuad(const renderer::OverlayVertex* vertices, usize quad) noexcept
{
    return vertices[quad * QUAD_VERTEX_COUNT].Coverage.Z == 1;
}
} // namespace

TEST_CASE("UiText draws labels after their element's fill and before later siblings", "[ui_text]")
{
    Fixture fixture;
    const Glyphs glyphs = MakeGlyphs();
    const core::StaticArray<ui_text::Label, 2> labels{{MakeLabel(BUTTON, glyphs), MakeLabel(CAPTION, glyphs)}};
    const ui_render::SurfaceMapping mapping = GetMapping(1);
    ui_text::UiTextReport report;
    REQUIRE(fixture.Append({labels.GetData(), labels.GetSize()}, mapping, report) == Status::Ready);
    // Panel, button, sibling fills; the hidden element paints nothing.
    CHECK(report.Commands == 3);
    CHECK(report.FillQuads == 3);
    CHECK(report.CulledFills == 0);
    CHECK(report.Labels == 2);
    CHECK(report.HiddenLabels == 0);
    CHECK(report.GlyphQuads == 2 * GLYPH_COUNT);
    CHECK(report.CulledGlyphs == 0);
    // Panel (viewport scissor); button fill, button text and caption text (panel scissor); sibling.
    CHECK(report.Runs == 3);
    CHECK(report.TextRuns == 1);
    const usize quads = report.FillQuads + report.GlyphQuads;
    REQUIRE(fixture.List.VertexCount() == quads * QUAD_VERTEX_COUNT);
    REQUIRE(fixture.List.CommandCount() == 3);
    const renderer::OverlayCommand* commands = fixture.List.Commands();
    CHECK(commands[1].Count == (1 + 2 * GLYPH_COUNT) * QUAD_VERTEX_COUNT);
    CHECK(commands[1].Style.Clip == true);
    CHECK(commands[1].Style.Scissor.X == 4);
    CHECK(commands[1].Style.Scissor.Width == 30);
    const renderer::OverlayVertex* vertices = fixture.List.Vertices();
    // Paint order: panel, button, button glyphs, caption glyphs, sibling.
    CHECK(IsCoverageQuad(vertices, 0) == false);
    CHECK(IsCoverageQuad(vertices, 1) == false);
    for (usize glyph = 0; glyph < 2 * GLYPH_COUNT; ++glyph)
    {
        CHECK(IsCoverageQuad(vertices, 2 + glyph) == true);
    }
    CHECK(IsCoverageQuad(vertices, quads - 1) == false);
    CHECK(vertices[(quads - 1) * QUAD_VERTEX_COUNT].Color.Z == 1);
    // The button's layout box starts at (6, 6); the label pen starts one pixel in and its baseline
    // is seven pixels down, so the first glyph's top-left is (7, 7).
    CHECK(IsQuadAt(vertices, 2, mapping, 7, 7) == true);
    CHECK(IsQuadAt(vertices, 3, mapping, 7 + GLYPH_PITCH, 7) == true);
    // The caption box starts at (6, 18).
    CHECK(IsQuadAt(vertices, 2 + GLYPH_COUNT, mapping, 7, 19) == true);
    // 30 x 24 + 20 x 10 + 8 x 30 fill pixels plus six 4 x 6 glyphs.
    CHECK(report.ShadedPixels == 720 + 200 + 240 + 2 * GLYPH_COUNT * 24);
}

TEST_CASE("UiText fills, runs and vertices equal UiRender when no labels are attached", "[ui_text]")
{
    for (const float32 scale : {1.0F, 1.5F, 2.0F})
    {
        Fixture fixture;
        renderer::OverlayList reference;
        REQUIRE(reference.Initialize() == Status::Ready);
        const ui_render::SurfaceMapping mapping = GetMapping(scale);
        ui_render::PaintReport paint;
        REQUIRE(ui_render::AppendPaint(reference, fixture.Context.GetPaintCommands(), mapping, paint) == Status::Ready);
        ui_text::UiTextReport report;
        REQUIRE(fixture.Append({}, mapping, report) == Status::Ready);
        CHECK(report.FillQuads == paint.Quads);
        CHECK(report.CulledFills == paint.Culled);
        CHECK(report.Runs == paint.Runs);
        CHECK(report.TextRuns == 0);
        REQUIRE(fixture.List.VertexCount() == reference.VertexCount());
        REQUIRE(fixture.List.CommandCount() == reference.CommandCount());
        for (usize index = 0; index < reference.VertexCount(); ++index)
        {
            const renderer::OverlayVertex& actual = fixture.List.Vertices()[index];
            const renderer::OverlayVertex& expected = reference.Vertices()[index];
            CHECK(actual.Clip.X == expected.Clip.X);
            CHECK(actual.Clip.Y == expected.Clip.Y);
            CHECK(actual.Color.X == expected.Color.X);
        }
        for (usize index = 0; index < reference.CommandCount(); ++index)
        {
            CHECK(renderer::CanMergeOverlayStyles(fixture.List.Commands()[index].Style,
                                                  reference.Commands()[index].Style) == true);
        }
    }
}

TEST_CASE("UiText scissors labels to the element's effective clip and culls glyphs outside it", "[ui_text]")
{
    Fixture fixture;
    // One glyph inside the panel clip, one straddling its right edge at x = 34 and one past it.
    const core::StaticArray<gt::TextQuad, 3> glyphs{{
        {{0, -6, 4, 0}, {0, 0, 0.125F, 0.125F}, 0},
        {{25, -6, 29, 0}, {0.125F, 0, 0.25F, 0.125F}, 1},
        {{40, -6, 44, 0}, {0.25F, 0, 0.375F, 0.125F}, 2},
    }};
    const ui_text::Label label = MakeLabel(BUTTON, {glyphs.GetData(), glyphs.GetSize()});
    ui_text::UiTextReport report;
    REQUIRE(fixture.Append({&label, 1}, GetMapping(1), report) == Status::Ready);
    // The button box starts at x = 6 and the pen one pixel later: glyph one spans [32, 36), so two of
    // its columns lie inside the panel clip [4, 34); glyph two at [47, 51) lies wholly outside.
    CHECK(report.GlyphQuads == 2);
    CHECK(report.CulledGlyphs == 1);
    CHECK(report.ShadedPixels == 720 + 200 + 240 + 24 + 2 * 6);
    const renderer::OverlayCommand& run = fixture.List.Commands()[1];
    CHECK(run.Style.Scissor.X + run.Style.Scissor.Width == 34);
}

TEST_CASE("UiText skips labels of hidden or absent elements but still validates them", "[ui_text]")
{
    Fixture fixture;
    const Glyphs glyphs = MakeGlyphs();
    core::StaticArray<ui_text::Label, 2> labels{{MakeLabel(HIDDEN, glyphs), MakeLabel(MISSING, glyphs)}};
    ui_text::UiTextReport report;
    REQUIRE(fixture.Append({labels.GetData(), labels.GetSize()}, GetMapping(1), report) == Status::Ready);
    CHECK(report.Labels == 0);
    CHECK(report.HiddenLabels == 2);
    CHECK(report.GlyphQuads == 0);
    CHECK(report.TextRuns == 0);
    fixture.List.Clear();
    labels[1].Color.R = 2;
    CHECK(fixture.Append({labels.GetData(), labels.GetSize()}, GetMapping(1), report) == Status::InvalidDescription);
    CHECK(fixture.List.VertexCount() == 0);
}

TEST_CASE("UiText places baselines through the mapping's scale and origin", "[ui_text]")
{
    Fixture fixture;
    const Glyphs glyphs = MakeGlyphs();
    ui_text::Label label = MakeLabel(BUTTON, glyphs);
    label.Scale = 2;
    constexpr float32 ORIGIN_X = 3;
    constexpr float32 ORIGIN_Y = 5;
    ui_render::SurfaceMapping mapping = GetMapping(2);
    mapping.OriginX = ORIGIN_X;
    mapping.OriginY = ORIGIN_Y;
    ui_text::UiTextReport report;
    REQUIRE(fixture.Append({&label, 1}, mapping, report) == Status::Ready);
    REQUIRE(report.GlyphQuads == GLYPH_COUNT);
    // Logical baseline origin (7, 13) maps to physical (3 + 14, 5 + 26); glyph quads stay physical.
    CHECK(IsQuadAt(fixture.List.Vertices(), 2, mapping, ORIGIN_X + 14, ORIGIN_Y + 26 - GLYPH_ASCENT) == true);
}

TEST_CASE("UiText failures leave the list and report unchanged", "[ui_text]")
{
    Fixture fixture;
    const Glyphs glyphs = MakeGlyphs();
    const ui_render::SurfaceMapping mapping = GetMapping(1);
    // Existing geometry the failed calls must preserve.
    REQUIRE(fixture.List.Quad({0, 0, 1, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, mapping.Width, mapping.Height) ==
            Status::Ready);
    const usize vertices = fixture.List.VertexCount();
    const usize commands = fixture.List.CommandCount();
    ui_text::UiTextReport sentinel;
    sentinel.Runs = 77;
    const auto expectFailure = [&](std::span<const ui_text::Label> labels,
                                   std::span<const ui::PaintCommand> paint,
                                   const ui_render::SurfaceMapping& surface,
                                   Status expected) {
        ui_text::UiTextReport report = sentinel;
        CHECK(ui_text::AppendUi(fixture.List, fixture.Context.GetLayout(), paint, labels, surface, report) == expected);
        CHECK(report.Runs == sentinel.Runs);
        CHECK(fixture.List.VertexCount() == vertices);
        CHECK(fixture.List.CommandCount() == commands);
        CHECK(fixture.List.DroppedCommands() == 0);
    };
    const std::span<const ui::PaintCommand> paint = fixture.Context.GetPaintCommands();
    ui_text::Label label = MakeLabel(BUTTON, glyphs);

    SECTION("Invalid mapping")
    {
        ui_render::SurfaceMapping invalid = mapping;
        invalid.Scale = 0;
        expectFailure({&label, 1}, paint, invalid, Status::InvalidDescription);
    }
    SECTION("Label prepared for another scale")
    {
        label.Scale = 2;
        expectFailure({&label, 1}, paint, mapping, Status::InvalidDescription);
    }
    SECTION("Zero element")
    {
        label.Element = 0;
        expectFailure({&label, 1}, paint, mapping, Status::InvalidDescription);
    }
    SECTION("Nonfinite offset")
    {
        label.OffsetX = std::numeric_limits<float32>::infinity();
        expectFailure({&label, 1}, paint, mapping, Status::InvalidDescription);
    }
    SECTION("Glyph UV outside the atlas")
    {
        Glyphs invalid = glyphs;
        invalid[2].Uv.Z = 2;
        label.Glyphs = {invalid.GetData(), invalid.GetSize()};
        expectFailure({&label, 1}, paint, mapping, Status::InvalidDescription);
    }
    SECTION("Unordered glyph rectangle")
    {
        Glyphs invalid = glyphs;
        invalid[1].Rectangle.Z = invalid[1].Rectangle.X - 1;
        label.Glyphs = {invalid.GetData(), invalid.GetSize()};
        expectFailure({&label, 1}, paint, mapping, Status::InvalidDescription);
    }
    SECTION("Paint commands from another document")
    {
        const core::StaticArray<ui::PaintCommand, 2> reordered{{paint[1], paint[0]}};
        expectFailure({&label, 1}, {reordered.GetData(), reordered.GetSize()}, mapping, Status::InvalidDescription);
    }
    SECTION("Glyphs exceed the remaining vertex capacity")
    {
        constexpr usize MANY = renderer::OverlayList::MAX_VERTICES / QUAD_VERTEX_COUNT;
        static core::StaticArray<gt::TextQuad, MANY> many;
        for (gt::TextQuad& glyph : many)
        {
            glyph = glyphs[0];
        }
        label.Glyphs = {many.GetData(), many.GetSize()};
        expectFailure({&label, 1}, paint, mapping, Status::CapacityExceeded);
    }
}

TEST_CASE("UiText rejects an uninitialized list", "[ui_text]")
{
    Fixture fixture;
    renderer::OverlayList list;
    ui_text::UiTextReport report;
    CHECK(ui_text::AppendUi(list,
                            fixture.Context.GetLayout(),
                            fixture.Context.GetPaintCommands(),
                            {},
                            GetMapping(1),
                            report) == Status::NotReady);
}
