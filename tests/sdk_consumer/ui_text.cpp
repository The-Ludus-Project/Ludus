// Installed-SDK U1 text qualification for Ludus::UiText (issue #1219).
//
// A clipped panel holds a button with a label and a transparent caption whose label overflows the
// panel's right edge; a sibling button painted afterwards covers part of both labels. Each case
// draws offscreen through UiText -> OverlayList -> OverlayRenderer with the published coverage
// atlas, reads the RGBA8 target back and checks every pixel against an independent composite of the
// snapped fills and the atlas coverage under the element clips. Natively the labels are shaped from
// a packaged font through LabelCache; two more cases reject an atlas-exhausting label and rebuild
// every GPU object after destroying the device, and both must reproduce the last frame's bytes. The
// browser has no native shaping, so it draws copied CPU glyph fixtures through the same adapter.
// Contract: docs/architecture/ui.md#u1-text-adapter.

#include "renderer_overlay.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/graphics/text/coverage_atlas.hpp>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/graphics/ui_text/ui_text.hpp>
#include <ludus/ui/context.h>

#include <span>
#include <string_view>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#else
#    include <ludus/foundation/containers/array.hpp>
#    include <ludus/foundation/containers/span_compatibility.hpp>
#    include <ludus/foundation/filesystem/filesystem.hpp>
#    include <ludus/graphics/ui_text/label_cache.hpp>
#    include <ludus/text/font_system.h>
#endif

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace gt = ludus::graphics::text;
namespace ui = ludus::ui;
namespace ui_render = ludus::graphics::ui_render;
namespace ui_text = ludus::graphics::ui_text;
using Status = rhi::RasterStatus;

constexpr logging::LogCategory LOG_UI_TEXT{"UiTextFixture"};
constexpr uint32 SURFACE_WIDTH = 96;
constexpr uint32 SURFACE_HEIGHT = 64;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize MAX_IMAGE_BYTES = usize{SURFACE_WIDTH} * SURFACE_HEIGHT * CHANNEL_COUNT;
constexpr float64 PIXEL_CENTER = 0.5;
constexpr float64 UNORM_MAX = 255;
/// Coverage blends an 8-bit mask through the backend's UNorm rounding; allow two bytes.
constexpr float64 BYTE_TOLERANCE = 2;
#if !defined(LUDUS_PLATFORM_WEB)
/// Native runs tick until the fixture finishes or this many seconds pass; the browser's main loop has no limit.
constexpr int64 TIME_LIMIT_SECONDS = 30;
#endif
using Pixels = core::StaticArray<uint8, MAX_IMAGE_BYTES>;

constexpr ui::WidgetId PANEL = 1;
constexpr ui::WidgetId BUTTON = 2;
constexpr ui::WidgetId CAPTION = 3;
constexpr ui::WidgetId SIBLING = 4;
constexpr usize ELEMENT_COUNT = 4;
constexpr usize LABEL_COUNT = 2;
using Document = core::StaticArray<ui::Element, ELEMENT_COUNT>;
constexpr ui::Color RED{1, 0, 0, 1};
constexpr ui::Color BLUE{0, 0, 1, 1};
constexpr ui::Color GRAY{0.25F, 0.25F, 0.25F, 1};
constexpr ui::Color WHITE{1, 1, 1, 1};
constexpr ui::Color YELLOW{1, 1, 0, 1};
constexpr float32 PANEL_OFFSET = 4;
constexpr float32 PANEL_WIDTH = 40; // Right clip edge at logical 44: the caption label overflows it.
constexpr float32 PANEL_HEIGHT = 20;
constexpr float32 CHILD_OFFSET = 2;
constexpr float32 BUTTON_WIDTH = 24;
constexpr float32 BUTTON_HEIGHT = 10;
constexpr float32 CAPTION_OFFSET_Y = 13;
constexpr float32 CAPTION_WIDTH = 38;
constexpr float32 CAPTION_HEIGHT = 7;
constexpr float32 SIBLING_OFFSET_X = 20;
constexpr float32 SIBLING_OFFSET_Y = 9;
constexpr float32 SIBLING_WIDTH = 6;
constexpr float32 SIBLING_HEIGHT = 18;
/// Logical label metrics: whole logical pixels keep every physical baseline on a pixel boundary.
constexpr float32 LABEL_OFFSET_X = 1;
constexpr float32 LABEL_BASELINE_Y = 7;
constexpr uint32 LABEL_LOGICAL_PIXELS = 8;

enum class CaseKind : uint8
{
    /// Prepare labels for the case's scale, draw them and check every pixel.
    Draw,
    /// Request a label the atlas cannot hold; the last ready labels must draw unchanged.
    Exhausted,
    /// Destroy the device and rebuild every GPU object from the CPU labels and atlas shadow.
    DeviceLoss,
};

struct TextCase final
{
    std::string_view Name;
    float32 Scale = 1;
    uint32 Width = 0;
    uint32 Height = 0;
    CaseKind Kind = CaseKind::Draw;
};
#if defined(LUDUS_PLATFORM_WEB)
constexpr usize CASE_COUNT = 3;
#else
constexpr usize CASE_COUNT = 5;
#endif
/// Cases after the third reuse its mapping and must reproduce its bytes.
constexpr usize REFERENCE_CASE = 2;
constexpr core::StaticArray<TextCase, CASE_COUNT> CASES{{
    {"scale 1", 1, 48, 32, CaseKind::Draw},
    {"scale 2", 2, 96, 64, CaseKind::Draw},
    {"scale 2 after resize", 2, 80, 60, CaseKind::Draw},
#if !defined(LUDUS_PLATFORM_WEB)
    {"atlas exhaustion keeps the last ready labels", 2, 80, 60, CaseKind::Exhausted},
    {"rebuilt after device loss", 2, 80, 60, CaseKind::DeviceLoss},
#endif
}};

enum class Step : uint8
{
    StartDevice,
    WaitDevice,
    StartOverlays,
    WaitOverlays,
    PrepareCase,
    WaitCase,
    Draw,
    ReadPixels,
    Complete,
};

rhi::WindowInfo gWindow;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rr::OverlayRenderer gOverlays;
rr::OverlayList gList;
ui::Context gContext;
rr::PreparedOverlay gOverlay;
core::StaticArray<rhi::TextureHandle, CASE_COUNT> gTargets;
rhi::TextureHandle gAtlasTexture;
/// Atlas revision gAtlasTexture holds; a new texture is published when the labels need a newer one.
uint64 gPublishedRevision = 0;
bool hasPublishedAtlas = false;
rhi::ReadbackTicket gReadback;
Step gStep = Step::StartDevice;
usize gCaseIndex = 0;
ui_text::UiTextReport gReport;
core::StaticArray<uint64, CASE_COUNT> gHashes;
/// Globals, not Tick locals: the browser main loop runs on a 64 KiB stack.
Pixels gPixels;
/// The last checked frame, kept while a replacement is prepared so a failure never erases it.
Pixels gLastFrame;
bool isFixtureDone = false;
bool hasFixtureFailed = false;

#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gPlatformWindow;
// clang-format off
EM_JS(int32, SelectBackend, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(void, ReportCase, (int32 index, uint32 scale, uint32 width, uint32 height, uint32 runs, uint32 textRuns, uint32 glyphs, uint32 culled, uint32 hashHigh, uint32 hashLow), {
    const node = document.querySelector('#status');
    node.dataset['case' + index] = [scale, width, height, runs, textRuns, glyphs, culled, (hashHigh >>> 0).toString(16).padStart(8, '0') + (hashLow >>> 0).toString(16).padStart(8, '0')].join(',');
});
EM_JS(void, ReportState, (int32 state, int32 backend, uint32 cases), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : 'loading';
    Object.assign(node.dataset, {cases, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' cases=' + cases;
});
// clang-format on
#else
int32 SelectBackend() noexcept
{
    return 0;
}
void ReportCase(int32, uint32, uint32, uint32, uint32, uint32, uint32, uint32, uint32, uint32) noexcept {}
void ReportState(int32, int32, uint32) noexcept {}
#endif

[[nodiscard]] bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}

[[nodiscard]] bool IsWaiting(Status status) noexcept
{
    return status == Status::Pending || status == Status::NotReady;
}

[[nodiscard]] uint32 GetBackend() noexcept
{
    return static_cast<uint32>(rhi::GetStartup().SelectedBackend);
}

void Stop(bool hasFailed) noexcept
{
    isFixtureDone = true;
    hasFixtureFailed = hasFailed;
    constexpr int32 PASSED = 1;
    constexpr int32 FAILED = 2;
    ReportState(hasFailed == true ? FAILED : PASSED, static_cast<int32>(GetBackend()), static_cast<uint32>(gCaseIndex));
    if (hasFailed == true)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT, "UiText fixture failed at case {} on backend {}", gCaseIndex + 1, GetBackend());
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    gOverlays.Reset();
    rhi::Shutdown();
    static_cast<void>(logging::LogSystem::Shutdown());
}

[[nodiscard]] bool Fail(std::string_view operation, Status status) noexcept
{
    LUDUS_LOG_ERROR(LOG_UI_TEXT, "{} failed with raster status {}", operation, static_cast<uint32>(status));
    Stop(true);
    return false;
}

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
    button.Placement.OffsetX = CHILD_OFFSET;
    button.Placement.OffsetY = CHILD_OFFSET;
    button.Fill = RED;
    // Transparent: Ui emits no paint command, but its label still draws under the panel clip.
    ui::Element& caption = document[2];
    caption.Id = CAPTION;
    caption.Parent = 0;
    caption.Placement.Width = {CAPTION_WIDTH, ui::Unit::Pixels};
    caption.Placement.Height = {CAPTION_HEIGHT, ui::Unit::Pixels};
    caption.Placement.OffsetX = CHILD_OFFSET;
    caption.Placement.OffsetY = CAPTION_OFFSET_Y;
    ui::Element& sibling = document[3];
    sibling.Id = SIBLING;
    sibling.SemanticRole = ui::Role::Button;
    sibling.Placement.Width = {SIBLING_WIDTH, ui::Unit::Pixels};
    sibling.Placement.Height = {SIBLING_HEIGHT, ui::Unit::Pixels};
    sibling.Placement.OffsetX = SIBLING_OFFSET_X;
    sibling.Placement.OffsetY = SIBLING_OFFSET_Y;
    sibling.Fill = BLUE;
    return document;
}

[[nodiscard]] ui_render::SurfaceMapping GetMapping(const TextCase& test) noexcept
{
    return {test.Scale, 0, 0, test.Width, test.Height};
}

[[nodiscard]] uint32 GetRasterPixels(const TextCase& test) noexcept
{
    return static_cast<uint32>(static_cast<float32>(LABEL_LOGICAL_PIXELS) * test.Scale);
}

#if defined(LUDUS_PLATFORM_WEB)
// Copied CPU glyph fixtures: the browser build has no native shaping, so these stand in for
// PrepareText output. Each glyph is a 5 x 7 logical cell scaled to the raster size, with stems,
// a diagonal and a soft gradient so coverage blending is exercised away from 0 and 255.
constexpr usize FIXTURE_GLYPH_COUNT = 3;
constexpr usize MAX_FIXTURE_GLYPHS = 8;
constexpr uint32 FIXTURE_GLYPH_WIDTH = 5;
constexpr uint32 FIXTURE_GLYPH_HEIGHT = 7;
constexpr uint32 FIXTURE_ADVANCE = 6;
constexpr uint32 MAX_FIXTURE_SCALE = 2;
constexpr usize MAX_FIXTURE_BYTES =
    usize{FIXTURE_GLYPH_WIDTH} * FIXTURE_GLYPH_HEIGHT * MAX_FIXTURE_SCALE * MAX_FIXTURE_SCALE;
constexpr uint64 FIXTURE_FONT_VERSION = 1;
gt::CoverageAtlas gAtlas;
uint64 gAtlasRevision = 0;
core::StaticArray<core::StaticArray<gt::TextQuad, MAX_FIXTURE_GLYPHS>, LABEL_COUNT> gGlyphs;
core::StaticArray<ui_text::Label, LABEL_COUNT> gLabels;
core::StaticArray<uint8, MAX_FIXTURE_BYTES> gMask;

[[nodiscard]] uint8 GetFixtureTexel(uint32 glyph, uint32 x, uint32 y) noexcept
{
    constexpr uint8 FULL = 255;
    constexpr uint8 GRADIENT_STEP = 36;
    if (glyph == 0)
    {
        return x == 0 || x == FIXTURE_GLYPH_WIDTH - 1 || y == FIXTURE_GLYPH_HEIGHT / 2 ? FULL : 0; // "H"
    }
    if (glyph == 1)
    {
        return x == y * (FIXTURE_GLYPH_WIDTH - 1) / (FIXTURE_GLYPH_HEIGHT - 1) ? FULL : 0; // diagonal
    }
    return static_cast<uint8>(GRADIENT_STEP * (y + 1)); // soft block
}

/// Insert the three fixture glyphs at this raster size and lay out both labels.
[[nodiscard]] bool PrepareLabels(const TextCase& test) noexcept
{
    if (gAtlas.Initialize() != Status::Ready)
    {
        return false;
    }
    const uint32 scale = static_cast<uint32>(test.Scale);
    const uint32 width = FIXTURE_GLYPH_WIDTH * scale;
    const uint32 height = FIXTURE_GLYPH_HEIGHT * scale;
    const uint32 rasterPixels = GetRasterPixels(test);
    core::StaticArray<gt::AtlasGlyph, FIXTURE_GLYPH_COUNT> cells;
    const usize before = gAtlas.GlyphCount();
    for (uint32 glyph = 0; glyph < FIXTURE_GLYPH_COUNT; ++glyph)
    {
        for (uint32 y = 0; y < height; ++y)
        {
            for (uint32 x = 0; x < width; ++x)
            {
                gMask[usize{y} * width + x] = GetFixtureTexel(glyph, x / scale, y / scale);
            }
        }
        const int32 bearingTop = static_cast<int32>(height);
        if (gAtlas.Insert({FIXTURE_FONT_VERSION, glyph, rasterPixels},
                          gMask.GetData(),
                          usize{width} * height,
                          width,
                          height,
                          0,
                          bearingTop,
                          cells[glyph]) != Status::Ready)
        {
            return false;
        }
    }
    if (gAtlas.GlyphCount() != before)
    {
        ++gAtlasRevision;
    }
    // Button label: H, diagonal, block. Caption label: the same three glyphs repeated, wide enough
    // to overflow the panel clip.
    constexpr core::StaticArray<usize, LABEL_COUNT> LENGTHS{{FIXTURE_GLYPH_COUNT, MAX_FIXTURE_GLYPHS}};
    constexpr core::StaticArray<ui::WidgetId, LABEL_COUNT> ELEMENTS{{BUTTON, CAPTION}};
    constexpr core::StaticArray<ui::Color, LABEL_COUNT> COLORS{{WHITE, YELLOW}};
    for (usize label = 0; label < LABEL_COUNT; ++label)
    {
        for (usize index = 0; index < LENGTHS[label]; ++index)
        {
            const gt::AtlasGlyph& cell = cells[index % FIXTURE_GLYPH_COUNT];
            const float32 left = static_cast<float32>(index * FIXTURE_ADVANCE * scale);
            const float32 top = -static_cast<float32>(cell.BearingTop);
            gGlyphs[label][index] = {
                {left, top, left + static_cast<float32>(cell.Width), top + static_cast<float32>(cell.Height)},
                cell.Uv,
                static_cast<uint32>(index)};
        }
        ui_text::Label& placed = gLabels[label];
        placed.Element = ELEMENTS[label];
        placed.Glyphs = {gGlyphs[label].GetData(), LENGTHS[label]};
        placed.OffsetX = LABEL_OFFSET_X;
        placed.BaselineY = LABEL_BASELINE_Y;
        placed.Color = COLORS[label];
        placed.Scale = test.Scale;
    }
    return true;
}

[[nodiscard]] std::span<const ui_text::Label> GetLabels() noexcept
{
    return {gLabels.GetData(), gLabels.GetSize()};
}

[[nodiscard]] const gt::CoverageAtlas& GetAtlas() noexcept
{
    return gAtlas;
}

[[nodiscard]] uint64 GetAtlasRevision() noexcept
{
    return gAtlasRevision;
}
#else
namespace cpu = ludus::text;
constexpr std::string_view FONT_FILE = "NotoSans-Regular.ttf";
constexpr uint64 FONT_VERSION = 1;
constexpr std::string_view BUTTON_TEXT = "Ok go";
constexpr std::string_view CAPTION_TEXT = "Ludus labels clip";
/// Two glyphs at the largest raster size cannot share one atlas page.
constexpr std::string_view EXHAUSTING_TEXT = "MW";
constexpr uint32 EXHAUSTING_PIXELS = 256;
cpu::FontSystem* gFontsOrNull = nullptr;
cpu::FontId gFont;
ui_text::LabelCache gCache;
/// Font-system counters before the device-loss rebuild; shaping and rasterization must not rerun.
cpu::FontSystemCounters gCountersBeforeLoss;

[[nodiscard]] bool LoadFont() noexcept
{
    filesystem::Directory directory;
    filesystem::File file;
    Array<uint8> bytes;
    if (directory.Open(LUDUS_UI_TEXT_FONT_DIRECTORY).Succeeded() == false ||
        directory.OpenRead(FONT_FILE, file).Succeeded() == false ||
        bytes.TryResize(static_cast<usize>(file.Size())) == false)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT, "Cannot read {} from {}", FONT_FILE, LUDUS_UI_TEXT_FONT_DIRECTORY);
        return false;
    }
    const filesystem::ReadResult read = file.ReadAt(0, MakeSpan(bytes));
    return read.Outcome.Succeeded() == true && read.BytesRead == bytes.GetSize() &&
           cpu::CreateFontSystem({}, &gFontsOrNull) == cpu::Status::Ok &&
           cpu::LoadFont(gFontsOrNull, {bytes.GetData(), bytes.GetSize()}, 0, &gFont) == cpu::Status::Ok &&
           gCache.Initialize(gFontsOrNull) == Status::Ready;
}

/// Shape a run, copy it into the element's label and destroy the layout: the cache keeps no borrow.
[[nodiscard]] Status SetLabel(ui::WidgetId element,
                              std::string_view text,
                              uint32 pixelHeight,
                              float32 scale,
                              const ui::Color& color,
                              ui_text::LabelUpdate& outUpdate) noexcept
{
    cpu::LayoutId layout;
    if (cpu::ShapeLatin(gFontsOrNull, gFont, text, pixelHeight, &layout) != cpu::Status::Ok)
    {
        return Status::InvalidDescription;
    }
    ui_text::LabelSource source;
    source.Element = element;
    source.Layout = layout;
    source.FontVersion = FONT_VERSION;
    source.Scale = scale;
    source.OffsetX = LABEL_OFFSET_X;
    source.BaselineY = LABEL_BASELINE_Y;
    source.Color = color;
    const Status set = gCache.SetLabel(source, outUpdate);
    static_cast<void>(cpu::DestroyLayout(gFontsOrNull, layout));
    return set;
}

[[nodiscard]] bool PrepareLabels(const TextCase& test) noexcept
{
    ui_text::LabelUpdate update;
    if (test.Kind == CaseKind::Draw)
    {
        const uint32 pixels = GetRasterPixels(test);
        return SetLabel(BUTTON, BUTTON_TEXT, pixels, test.Scale, WHITE, update) == Status::Ready &&
               SetLabel(CAPTION, CAPTION_TEXT, pixels, test.Scale, YELLOW, update) == Status::Ready;
    }
    if (test.Kind == CaseKind::Exhausted)
    {
        const uint64 revision = gCache.GetAtlasRevision();
        const Status set = SetLabel(CAPTION, EXHAUSTING_TEXT, EXHAUSTING_PIXELS, test.Scale, YELLOW, update);
        LUDUS_LOG_INFO(LOG_UI_TEXT,
                       "Exhausting label: status {}, repair {}, {} requested glyphs, {} atlas glyphs, {} texels",
                       static_cast<uint32>(set),
                       static_cast<uint32>(update.Repair),
                       update.RequestedGlyphs,
                       update.AtlasGlyphs,
                       update.OccupiedTexels);
        return set == Status::CapacityExceeded && update.Repair == ui_text::LabelRepair::ReduceGlyphs &&
               update.HasCompacted == false && gCache.GetAtlasRevision() == revision;
    }
    // Device loss: the labels and CPU atlas survive; nothing is shaped or rasterized again.
    const cpu::FontSystemCounters counters = cpu::GetCounters(gFontsOrNull);
    return counters.ShapeCalls == gCountersBeforeLoss.ShapeCalls &&
           counters.RasterizeCalls == gCountersBeforeLoss.RasterizeCalls;
}

[[nodiscard]] std::span<const ui_text::Label> GetLabels() noexcept
{
    return gCache.GetLabels();
}

[[nodiscard]] const gt::CoverageAtlas& GetAtlas() noexcept
{
    return gCache.GetAtlas();
}

[[nodiscard]] uint64 GetAtlasRevision() noexcept
{
    return gCache.GetAtlasRevision();
}
#endif

[[nodiscard]] bool PrepareCase(const TextCase& test) noexcept
{
    const Document document = MakeDocument();
    const ui::Viewport viewport{
        {0, 0, static_cast<float32>(test.Width) / test.Scale, static_cast<float32>(test.Height) / test.Scale}};
    if (gContext.TrySetDocument({document.GetData(), document.GetSize()}, viewport) != ui::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT, "{}: Ui rejected the document", test.Name);
        Stop(true);
        return false;
    }
    if (PrepareLabels(test) == false)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT, "{}: preparing labels failed", test.Name);
        Stop(true);
        return false;
    }
    if (hasPublishedAtlas == false || GetAtlasRevision() != gPublishedRevision)
    {
        // Earlier overlay versions were released after their readback, so the old texture is unused.
        if (hasPublishedAtlas == true)
        {
            static_cast<void>(rhi::Destroy(gDevice, gAtlasTexture));
            gAtlasTexture = {};
        }
        const Status published = GetAtlas().Publish(gDevice, gAtlasTexture);
        if (IsAccepted(published) == false)
        {
            return Fail("publishing atlas", published);
        }
        hasPublishedAtlas = true;
        gPublishedRevision = GetAtlasRevision();
    }
    gList.Clear();
    const Status appended = ui_text::AppendUi(gList,
                                              gContext.GetLayout(),
                                              gContext.GetPaintCommands(),
                                              GetLabels(),
                                              GetMapping(test),
                                              gReport);
    if (appended != Status::Ready)
    {
        return Fail("appending Ui text", appended);
    }
    // Panel (viewport scissor); button fill, button text and caption text (panel scissor); sibling
    // (viewport scissor). Both labels share one text run with the button fill.
    constexpr usize EXPECTED_FILLS = 3;
    constexpr usize EXPECTED_RUNS = 3;
    if (gReport.FillQuads != EXPECTED_FILLS || gReport.Runs != EXPECTED_RUNS || gReport.TextRuns != 1 ||
        gReport.Labels != LABEL_COUNT || gReport.GlyphQuads == 0 || gReport.CulledGlyphs == 0)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT,
                        "{}: {} fills, {} runs, {} text runs, {} labels, {} glyphs, {} culled glyphs",
                        test.Name,
                        gReport.FillQuads,
                        gReport.Runs,
                        gReport.TextRuns,
                        gReport.Labels,
                        gReport.GlyphQuads,
                        gReport.CulledGlyphs);
        Stop(true);
        return false;
    }
    const Status prepared = gOverlays.Prepare(gList, gAtlasTexture, gOverlay);
    if (IsAccepted(prepared) == false)
    {
        return Fail("preparing overlay", prepared);
    }
    return true;
}

[[nodiscard]] Status DrawCase(const TextCase& test) noexcept
{
    const rhi::DeviceStatus begun = rhi::BeginFrame(gDevice, gSurface);
    if (begun == rhi::DeviceStatus::Skipped)
    {
        // The surface skipped this frame; retry the whole case on the next tick.
        return Status::NotReady;
    }
    if (begun != rhi::DeviceStatus::Ready)
    {
        return Status::Failed;
    }
    rhi::RasterPassDescription pass;
    pass.Color = gTargets[gCaseIndex];
    // OverlayRenderer reopens the target once per run with read-only depth loaded, so the clearing
    // pass must store the depth it initializes.
    pass.DepthStore = rhi::RasterStore::Store;
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        pass.Clear[channel] = 0;
    }
    const Status begunPass = rhi::BeginRasterPass(gDevice, pass);
    const Status drawn = begunPass == Status::Ready ? gOverlays.Draw(gOverlay, pass) : begunPass;
    rhi::SubmissionToken completion;
    const Status submitted = rhi::EndFrame(gDevice, completion);
    if (drawn != Status::Ready)
    {
        return drawn;
    }
    if (submitted != Status::Ready)
    {
        return submitted;
    }
    const usize rowBytes = usize{test.Width} * CHANNEL_COUNT;
    return rhi::RequestTextureReadback(gDevice,
                                       gTargets[gCaseIndex],
                                       { .Width = test.Width, .Height = test.Height },
                                       rowBytes,
                                       gReadback);
}

/// FNV-1a 64 of the readback, published so the browser harness can compare backends bytewise.
[[nodiscard]] uint64 HashPixels(std::span<const uint8> bytes) noexcept
{
    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;
    uint64 hash = FNV_OFFSET;
    for (const uint8 byte : bytes)
    {
        hash = (hash ^ byte) * FNV_PRIME;
    }
    return hash;
}

/// Straight-alpha color composited premultiplied over a premultiplied destination, as the overlay
/// shader and blend state do in the linear target.
struct Composite final
{
    core::StaticArray<float64, CHANNEL_COUNT> Channels{};
    /// Whether any label contributed nonzero coverage to the pixel.
    bool HasInk = false;

    void Blend(const ui::Color& color, float64 coverage) noexcept
    {
        const float64 alpha = static_cast<float64>(color.A) * coverage;
        const core::StaticArray<float64, CHANNEL_COUNT> source{{static_cast<float64>(color.R) * alpha,
                                                                static_cast<float64>(color.G) * alpha,
                                                                static_cast<float64>(color.B) * alpha,
                                                                alpha}};
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            Channels[channel] = source[channel] + Channels[channel] * (1 - alpha);
        }
    }
};

[[nodiscard]] bool IsInside(const rhi::RasterRectangle& rectangle, uint32 x, uint32 y) noexcept
{
    return x >= rectangle.X && x < rectangle.X + rectangle.Width && y >= rectangle.Y &&
           y < rectangle.Y + rectangle.Height;
}

/// Bilinear sample of the R8 atlas shadow at a texel-space position, as the overlay sampler filters.
/// Every cell has a transparent gutter, so the four taps stay inside the page.
[[nodiscard]] float64 SampleAtlas(const uint8* coverage, float64 u, float64 v) noexcept
{
    const float64 x = u - PIXEL_CENTER;
    const float64 y = v - PIXEL_CENTER;
    const float64 left = static_cast<float64>(static_cast<int64>(x));
    const float64 top = static_cast<float64>(static_cast<int64>(y));
    const float64 fractionX = x - left;
    const float64 fractionY = y - top;
    const usize column = static_cast<usize>(left);
    const usize row = static_cast<usize>(top);
    const auto texel = [coverage](usize texelX, usize texelY) {
        return static_cast<float64>(coverage[texelY * gt::CoverageAtlas::EXTENT + texelX]) / UNORM_MAX;
    };
    const float64 upper = texel(column, row) * (1 - fractionX) + texel(column + 1, row) * fractionX;
    const float64 lower = texel(column, row + 1) * (1 - fractionX) + texel(column + 1, row + 1) * fractionX;
    return upper * (1 - fractionY) + lower * fractionY;
}

/// Composite one pixel from the paint commands, the labels and the CPU atlas shadow.
[[nodiscard]] Composite ComputeExpected(const TextCase& test, uint32 x, uint32 y) noexcept
{
    const ui_render::SurfaceMapping mapping = GetMapping(test);
    const std::span<const ui::PaintCommand> commands = gContext.GetPaintCommands();
    const uint8* coverage = GetAtlas().Coverage();
    Composite composite;
    usize nextCommand = 0;
    for (const ui::LayoutBox& box : gContext.GetLayout())
    {
        if (nextCommand < commands.size() && commands[nextCommand].Id == box.Id)
        {
            rhi::RasterRectangle fill;
            if (ui_render::TryComputePhysicalRectangle(commands[nextCommand].Bounds, mapping, fill) == true &&
                IsInside(fill, x, y) == true)
            {
                composite.Blend(commands[nextCommand].Fill, 1);
            }
            ++nextCommand;
        }
        rhi::RasterRectangle clip;
        if (box.Visible == false || ui_render::TryComputePhysicalRectangle(box.Clip, mapping, clip) == false ||
            IsInside(clip, x, y) == false)
        {
            continue;
        }
        for (const ui_text::Label& label : GetLabels())
        {
            if (label.Element != box.Id)
            {
                continue;
            }
            const float64 baselineX = (static_cast<float64>(box.Bounds.X) + static_cast<float64>(label.OffsetX)) *
                                      static_cast<float64>(test.Scale);
            const float64 baselineY = (static_cast<float64>(box.Bounds.Y) + static_cast<float64>(label.BaselineY)) *
                                      static_cast<float64>(test.Scale);
            const float64 centreX = static_cast<float64>(x) + PIXEL_CENTER;
            const float64 centreY = static_cast<float64>(y) + PIXEL_CENTER;
            for (const gt::TextQuad& glyph : label.Glyphs)
            {
                const float64 left = baselineX + static_cast<float64>(glyph.Rectangle.X);
                const float64 top = baselineY + static_cast<float64>(glyph.Rectangle.Y);
                const float64 right = baselineX + static_cast<float64>(glyph.Rectangle.Z);
                const float64 bottom = baselineY + static_cast<float64>(glyph.Rectangle.W);
                // The rasterizer covers pixel centres inside the half-open rectangle.
                if (centreX < left || centreX >= right || centreY < top || centreY >= bottom)
                {
                    continue;
                }
                // Glyph rectangles span their cell one texel per pixel, so the UV interpolates
                // linearly from the cell's top-left texel edge.
                const float64 extent = static_cast<float64>(gt::CoverageAtlas::EXTENT);
                const float64 u =
                    static_cast<float64>(glyph.Uv.X) * extent +
                    (centreX - left) * (static_cast<float64>(glyph.Uv.Z - glyph.Uv.X) * extent) / (right - left);
                const float64 v =
                    static_cast<float64>(glyph.Uv.Y) * extent +
                    (centreY - top) * (static_cast<float64>(glyph.Uv.W - glyph.Uv.Y) * extent) / (bottom - top);
                const float64 sampled = SampleAtlas(coverage, u, v);
                composite.HasInk = composite.HasInk == true || sampled > 0;
                composite.Blend(label.Color, sampled);
            }
        }
    }
    return composite;
}

[[nodiscard]] bool IsNear(const uint8* pixel, const Composite& expected) noexcept
{
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const float64 difference = static_cast<float64>(pixel[channel]) - expected.Channels[channel] * UNORM_MAX;
        if (difference < -BYTE_TOLERANCE || difference > BYTE_TOLERANCE)
        {
            return false;
        }
    }
    return true;
}

/// Check every pixel against the composite and require text actually reached the frame.
[[nodiscard]] bool CheckCase(const TextCase& test, std::span<const uint8> pixels) noexcept
{
    usize mismatches = 0;
    usize textPixels = 0;
    for (uint32 y = 0; y < test.Height; ++y)
    {
        for (uint32 x = 0; x < test.Width; ++x)
        {
            const Composite expected = ComputeExpected(test, x, y);
            const uint8* pixel = pixels.data() + (usize{y} * test.Width + x) * CHANNEL_COUNT;
            textPixels += expected.HasInk == true ? 1 : 0;
            if (IsNear(pixel, expected) == false)
            {
                if (mismatches == 0)
                {
                    LUDUS_LOG_ERROR(LOG_UI_TEXT,
                                    "{}: pixel ({}, {}) is {} {} {} {}, expected {} {} {} {}",
                                    test.Name,
                                    x,
                                    y,
                                    pixel[0],
                                    pixel[1],
                                    pixel[2],
                                    pixel[3],
                                    expected.Channels[0] * UNORM_MAX,
                                    expected.Channels[1] * UNORM_MAX,
                                    expected.Channels[2] * UNORM_MAX,
                                    expected.Channels[3] * UNORM_MAX);
                }
                ++mismatches;
            }
        }
    }
    // Probes: the overflow beyond the panel clip keeps the clear color, the sibling covers the
    // labels it overlaps with pure blue, and a press on the button's label activates the button.
    const uint32 scale = static_cast<uint32>(test.Scale);
    const uint32 overflowX = static_cast<uint32>(PANEL_OFFSET + PANEL_WIDTH) * scale;
    const uint32 overflowY = static_cast<uint32>(PANEL_OFFSET + CAPTION_OFFSET_Y + LABEL_BASELINE_Y - 1) * scale;
    usize probeFailures = 0;
    if (overflowX < test.Width)
    {
        const uint8* pixel = pixels.data() + (usize{overflowY} * test.Width + overflowX) * CHANNEL_COUNT;
        probeFailures += pixel[3] == 0 ? 0 : 1;
    }
    const uint32 siblingX = static_cast<uint32>(SIBLING_OFFSET_X + 1) * scale;
    const uint32 siblingY = static_cast<uint32>(PANEL_OFFSET + CHILD_OFFSET + LABEL_BASELINE_Y - 1) * scale;
    const uint8* sibling = pixels.data() + (usize{siblingY} * test.Width + siblingX) * CHANNEL_COUNT;
    constexpr uint8 FULL = 255;
    probeFailures += sibling[0] == 0 && sibling[1] == 0 && sibling[2] == FULL && sibling[3] == FULL ? 0 : 1;
    const float32 pressX = PANEL_OFFSET + CHILD_OFFSET + LABEL_OFFSET_X + 1;
    const float32 pressY = PANEL_OFFSET + CHILD_OFFSET + LABEL_BASELINE_Y - 1;
    static_cast<void>(gContext.Route({ui::EventKind::PointerDown, pressX, pressY, 0}));
    probeFailures += gContext.Route({ui::EventKind::PointerUp, pressX, pressY, 0}).Activated == BUTTON ? 0 : 1;

    const uint64 hash = HashPixels(pixels);
    gHashes[gCaseIndex] = hash;
    // A repeated case must reproduce the last ready frame, which it keeps until then, byte for byte.
    bool doesRepeatMatch = true;
    if (gCaseIndex > REFERENCE_CASE)
    {
        for (usize index = 0; index < pixels.size(); ++index)
        {
            doesRepeatMatch = doesRepeatMatch == true && pixels[index] == gLastFrame[index];
        }
    }
    constexpr uint32 HASH_HALF_BITS = 32;
    ReportCase(static_cast<int32>(gCaseIndex),
               scale,
               test.Width,
               test.Height,
               static_cast<uint32>(gReport.Runs),
               static_cast<uint32>(gReport.TextRuns),
               static_cast<uint32>(gReport.GlyphQuads),
               static_cast<uint32>(gReport.CulledGlyphs),
               static_cast<uint32>(hash >> HASH_HALF_BITS),
               static_cast<uint32>(hash));
    LUDUS_LOG_INFO(LOG_UI_TEXT,
                   "{} on backend {}: {}x{} physical, {} fills and {} glyphs ({} culled) in {} runs ({} with text), "
                   "{} shaded pixels, atlas {} glyphs / {} texels, {} text pixels, {} mismatches, {} probe failures, "
                   "hash {}{}",
                   test.Name,
                   GetBackend(),
                   test.Width,
                   test.Height,
                   gReport.FillQuads,
                   gReport.GlyphQuads,
                   gReport.CulledGlyphs,
                   gReport.Runs,
                   gReport.TextRuns,
                   gReport.ShadedPixels,
                   GetAtlas().GlyphCount(),
                   GetAtlas().OccupiedTexels(),
                   textPixels,
                   mismatches,
                   probeFailures,
                   hash,
                   doesRepeatMatch == true ? "" : " (differs from the last ready frame)");
    return mismatches == 0 && probeFailures == 0 && textPixels != 0 && doesRepeatMatch == true;
}

[[nodiscard]] bool StartDevice() noexcept
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const int32 selection = SelectBackend();
    description.Selection = selection == 1   ? rhi::BackendSelection::WebGPU
                            : selection == 2 ? rhi::BackendSelection::WebGL2
                                             : rhi::BackendSelection::Auto;
    rhi::StartupInfo startup;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, gWindow, description, gDevice, gSurface, &startup);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT,
                        "Device startup failed with status {}, error {}, requirement {}",
                        static_cast<uint32>(started),
                        static_cast<uint32>(startup.Error),
                        static_cast<uint32>(startup.UnmetRequirement));
        Stop(true);
        return false;
    }
    return true;
}

#if !defined(LUDUS_PLATFORM_WEB)
/// Drop every GPU object and the device, as device loss would, keeping the CPU labels, the atlas
/// shadow and the last checked frame.
[[nodiscard]] bool LoseDevice() noexcept
{
    gCountersBeforeLoss = cpu::GetCounters(gFontsOrNull);
    gOverlays.Reset();
    if (rhi::DestroyDevice(gDevice) != rhi::DeviceStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_UI_TEXT, "Destroying the device failed");
        Stop(true);
        return false;
    }
    gDevice = {};
    gSurface = {};
    gAtlasTexture = {};
    hasPublishedAtlas = false;
    for (rhi::TextureHandle& target : gTargets)
    {
        target = {};
    }
    return true;
}
#endif

void Tick() noexcept
{
    if (isFixtureDone == true)
    {
        return;
    }
    if (gStep == Step::StartDevice)
    {
        if (StartDevice() == false)
        {
            return;
        }
        gStep = Step::WaitDevice;
    }
    rhi::DeviceInfo info;
    const rhi::DeviceStatus device = rhi::GetDeviceInfo(gDevice, info);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStep == Step::WaitDevice)
    {
        gStep = Step::StartOverlays;
    }
    const Status lifetime = rhi::PollLifetime(gDevice);
    if (lifetime != Status::Ready)
    {
        static_cast<void>(Fail("polling lifetime", lifetime));
        return;
    }
    if (gStep == Step::StartOverlays)
    {
        const Status started = gOverlays.Initialize(gDevice,
                                                    ludus::shaders::renderer_overlay::Vertex(),
                                                    ludus::shaders::renderer_overlay::Fragment());
        if (IsAccepted(started) == false)
        {
            static_cast<void>(Fail("initializing overlays", started));
            return;
        }
        for (usize index = gCaseIndex; index < CASE_COUNT; ++index)
        {
            const Status created =
                rhi::CreateTexture(gDevice,
                                   {CASES[index].Width, CASES[index].Height, rhi::RasterFormat::Rgba8Unorm, true},
                                   {},
                                   gTargets[index]);
            if (IsAccepted(created) == false)
            {
                static_cast<void>(Fail("creating target", created));
                return;
            }
        }
        gStep = Step::WaitOverlays;
    }
    if (gStep == Step::WaitOverlays)
    {
        const Status polled = gOverlays.Poll();
        if (IsWaiting(polled) == true)
        {
            return;
        }
        if (polled != Status::Ready)
        {
            static_cast<void>(Fail("polling overlays", polled));
            return;
        }
        for (usize index = gCaseIndex; index < CASE_COUNT; ++index)
        {
            const Status status = rhi::GetStatus(gDevice, gTargets[index]);
            if (IsWaiting(status) == true)
            {
                return;
            }
            if (status != Status::Ready)
            {
                static_cast<void>(Fail("waiting for target", status));
                return;
            }
        }
        gStep = Step::PrepareCase;
    }
    const TextCase& test = CASES[gCaseIndex];
    if (gStep == Step::PrepareCase)
    {
        if (PrepareCase(test) == false)
        {
            return;
        }
        gStep = Step::WaitCase;
    }
    if (gStep == Step::WaitCase)
    {
        const Status atlas = rhi::GetStatus(gDevice, gAtlasTexture);
        const Status status = atlas == Status::Ready ? gOverlays.GetStatus(gOverlay) : atlas;
        if (IsWaiting(status) == true)
        {
            return;
        }
        if (status != Status::Ready)
        {
            static_cast<void>(Fail("waiting for atlas and prepared overlay", status));
            return;
        }
        gStep = Step::Draw;
    }
    if (gStep == Step::Draw)
    {
        const Status drawn = DrawCase(test);
        if (drawn == Status::NotReady)
        {
            return;
        }
        if (IsAccepted(drawn) == false)
        {
            static_cast<void>(Fail(test.Name, drawn));
            return;
        }
        gStep = Step::ReadPixels;
    }
    if (gStep == Step::ReadPixels)
    {
        const Status ready = rhi::GetStatus(gDevice, gReadback);
        if (IsWaiting(ready) == true)
        {
            return;
        }
        const usize imageBytes = usize{test.Width} * test.Height * CHANNEL_COUNT;
        const Status copied =
            ready == Status::Ready ? rhi::CopyReadback(gDevice, gReadback, gPixels.GetData(), imageBytes) : ready;
        if (copied != Status::Ready)
        {
            static_cast<void>(Fail("copying readback", copied));
            return;
        }
        const Status releasedReadback = rhi::Release(gDevice, gReadback);
        if (releasedReadback != Status::Ready)
        {
            static_cast<void>(Fail("releasing readback", releasedReadback));
            return;
        }
        const Status released = gOverlays.Release(gOverlay);
        if (released != Status::Ready)
        {
            static_cast<void>(Fail("releasing overlay", released));
            return;
        }
        if (CheckCase(test, {gPixels.GetData(), imageBytes}) == false)
        {
            Stop(true);
            return;
        }
        gLastFrame = gPixels;
        ++gCaseIndex;
        gStep = gCaseIndex == CASE_COUNT ? Step::Complete : Step::PrepareCase;
#if !defined(LUDUS_PLATFORM_WEB)
        if (gCaseIndex < CASE_COUNT && CASES[gCaseIndex].Kind == CaseKind::DeviceLoss)
        {
            if (LoseDevice() == false)
            {
                return;
            }
            gStep = Step::StartDevice;
            return;
        }
#endif
    }
    if (gStep == Step::Complete)
    {
        LUDUS_LOG_INFO(LOG_UI_TEXT, "{} UiText cases passed on backend {}", CASE_COUNT, GetBackend());
        Stop(false);
    }
}
} // namespace

int main()
{
    gWindow.Width = SURFACE_WIDTH;
    gWindow.Height = SURFACE_HEIGHT;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = SURFACE_WIDTH;
    settings.Height = SURFACE_HEIGHT;
    settings.CanvasSelector = "#canvas";
    settings.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(settings, gPlatformWindow) == false)
    {
        return 1;
    }
    gWindow = gPlatformWindow->GetNativeWindowInfo();
#endif
    static_cast<void>(logging::LogSystem::Initialize(
    {
        .EnableFile = false,
        .EnableDebugger = false,
        .Directory = {},
        .FlushIntervalMilliseconds = 0,
    }));
    if (gContext.TryInitialize(ELEMENT_COUNT) != ui::Status::Ok || gList.Initialize() != Status::Ready)
    {
        return 1;
    }
#if !defined(LUDUS_PLATFORM_WEB)
    if (LoadFont() == false)
    {
        return 1;
    }
#endif
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 start = time::NowTicks();
    while (isFixtureDone == false && time::NowTicks() - start < TIME_LIMIT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (isFixtureDone == false)
    {
        Stop(true);
    }
    static_cast<void>(cpu::DestroyFontSystem(gFontsOrNull));
#endif
    return hasFixtureFailed == true ? 1 : 0;
}
