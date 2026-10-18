#include "internal/text_demo.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include <ludus/text/font_system.h>

#include <cstdio>
#include <string>

// Where the example reads its font from. The native smoke CMake target injects
// LUDUS_SMOKE_FONT_PATH pointing at a bundled OFL font (NotoSans-Regular.ttf).
#ifndef LUDUS_SMOKE_FONT_PATH
#    define LUDUS_SMOKE_FONT_PATH ""
#endif

namespace ludus::smoke::text_demo
{
using namespace foundation;
using logging::LOG_CORE;

namespace
{
// Ludus::Text never reads files itself: the caller owns loading font bytes and
// passes them to LoadFont, which copies them into bounded owned storage. This
// helper reads the whole file into outBytes. A missing/unreadable file yields
// empty bytes; false reports that the byte buffer could not be allocated.
[[nodiscard]] bool ReadFileBytes(const char* path, Array<uint8>& outBytes) noexcept
{
    outBytes.Clear();
    if (path == nullptr || path[0] == '\0')
    {
        return true;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return true;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0)
    {
        if (outBytes.TryResize(static_cast<usize>(size)) == false)
        {
            std::fclose(file);
            return false;
        }
        const usize read = std::fread(outBytes.GetData(), 1, outBytes.GetSize(), file);
        outBytes.Resize(read); // shrinking never allocates
    }
    std::fclose(file);
    return true;
}

// Map an 8-bit coverage value (0..255) to a character for an ASCII-art preview
// of a rasterized glyph. Denser characters mean more ink.
char CoverageToChar(uint8 coverage) noexcept
{
    constexpr char kRamp[] = " .:-=+*#%@";
    constexpr int kLevels = static_cast<int>(sizeof(kRamp) - 2); // exclude NUL
    const int level = (static_cast<int>(coverage) * kLevels) / 255;
    return kRamp[level];
}

// Print a rasterized glyph's coverage mask as ASCII art, one log line per row.
// The mask is top-down, tightly packed R8 (Width is not a stride), so row r
// starts at offset r*Width.
void LogCoveragePreview(const text::GlyphBitmap& bitmap) noexcept
{
    if (bitmap.Width == 0 || bitmap.Height == 0)
    {
        LUDUS_LOG_INFO(LOG_CORE, "  (zero-area glyph: no coverage, e.g. a space)");
        return;
    }
    std::string row;
    row.reserve(bitmap.Width);
    for (uint32 y = 0; y < bitmap.Height; ++y)
    {
        row.clear();
        for (uint32 x = 0; x < bitmap.Width; ++x)
        {
            row.push_back(CoverageToChar(bitmap.Coverage[static_cast<usize>(y) * bitmap.Width + x]));
        }
        LUDUS_LOG_INFO(LOG_CORE, "  |{}|", row.c_str());
    }
}

bool RunDemo() noexcept
{
    // 1. Create a FontSystem. Passing a default-constructed config selects the
    //    documented budget defaults (8 fonts, 256 layouts, 64 MiB total, ...).
    text::FontSystem* system = nullptr;
    if (text::Status status = text::CreateFontSystem(text::FontSystemConfig{}, &system); text::IsOk(status) == false)
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: CreateFontSystem failed: {}", text::ToString(status));
        return false;
    }

    bool ok = true;

    // 2. Load font bytes ourselves and hand them to LoadFont (face index 0).
    Array<uint8> fontBytes;
    if (ReadFileBytes(LUDUS_SMOKE_FONT_PATH, fontBytes) == false)
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: out of memory reading '{}'", LUDUS_SMOKE_FONT_PATH);
        (void)text::DestroyFontSystem(system);
        return false;
    }
    if (fontBytes.IsEmpty())
    {
        LUDUS_LOG_WARN(LOG_CORE, "text demo: no font at '{}'; skipping font example", LUDUS_SMOKE_FONT_PATH);
        (void)text::DestroyFontSystem(system);
        return false;
    }

    text::FontId font;
    if (text::Status status = text::LoadFont(system, fontBytes, 0, &font); text::IsOk(status) == false)
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: LoadFont failed: {}", text::ToString(status));
        (void)text::DestroyFontSystem(system);
        return false;
    }

    uint32 glyphCount = 0;
    (void)text::GetGlyphCount(system, font, &glyphCount);
    LUDUS_LOG_INFO(LOG_CORE, "text demo: loaded font with {} outline glyphs", glyphCount);

    // 3. Shape a UTF-8 string into positioned glyphs. ShapeLatin is the Latin-LTR
    //    convenience entry point; ShapeRun takes a full RunSpec for other scripts
    //    and directions.
    constexpr const char* kText = "Ludus Text!";
    constexpr uint32 kPixelHeight = 32;
    text::LayoutId layout;
    if (text::Status status = text::ShapeLatin(system, font, kText, kPixelHeight, &layout); text::IsOk(status) == false)
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: ShapeLatin failed: {}", text::ToString(status));
        (void)text::UnloadFont(system, font);
        (void)text::DestroyFontSystem(system);
        return false;
    }

    // 4. Read the immutable layout: borrowed glyph + metric views valid until the
    //    layout (or system) is destroyed.
    text::LayoutView view;
    if (text::Status status = text::GetLayout(system, layout, &view); text::IsOk(status))
    {
        // Shaped positions are 26.6 signed fixed-point physical pixels: divide by
        // 64.0 to get fractional pixels.
        constexpr float32 kFixed = 64.0F;
        LUDUS_LOG_INFO(LOG_CORE,
                       "text demo: shaped \"{}\" at {}px -> {} glyphs, advance {:.1f}px, line {:.1f}px",
                       kText,
                       kPixelHeight,
                       view.Metrics.GlyphCount,
                       view.Metrics.AdvanceX,
                       view.Metrics.LineAdvance);
        LUDUS_LOG_INFO(LOG_CORE,
                       "text demo: ascent {:.1f}px descent {:.1f}px ink [{:.1f},{:.1f} .. {:.1f},{:.1f}]",
                       view.Metrics.Ascent,
                       view.Metrics.Descent,
                       view.Metrics.Ink.Left,
                       view.Metrics.Ink.Top,
                       view.Metrics.Ink.Right,
                       view.Metrics.Ink.Bottom);

        // Walk glyphs and accumulate the pen position from each XAdvance, exactly
        // as a GPU renderer would when placing textured quads. The per-glyph x
        // position is computed inline in the log call so it is not an unused
        // local in builds where LUDUS_LOG_INFO compiles out (e.g. Release).
        [[maybe_unused]] float32 penX = 0.0F;
        for (usize i = 0; i < view.Glyphs.size(); ++i)
        {
            const text::ShapedGlyph& glyph = view.Glyphs[i];
            LUDUS_LOG_INFO(LOG_CORE,
                           "  glyph[{}] id={} cluster={} penX={:.1f} advance={:.1f}{}",
                           i,
                           glyph.GlyphId,
                           glyph.Cluster,
                           penX + static_cast<float32>(glyph.XOffset) / kFixed,
                           static_cast<float32>(glyph.XAdvance) / kFixed,
                           glyph.IsMissing ? " (missing/.notdef)" : "");
            penX += static_cast<float32>(glyph.XAdvance) / kFixed;
        }

        // 5. Rasterize the first visible glyph into an R8 coverage mask and show
        //    it as ASCII art. The returned Coverage span is borrowed scratch,
        //    valid only until the next raster call, so consume it right away.
        if (view.Glyphs.empty() == false)
        {
            const text::ShapedGlyph& first = view.Glyphs.front();
            text::GlyphBitmap bitmap;
            if (text::Status raster = text::RasterizeGlyph(system, font, kPixelHeight, first.GlyphId, &bitmap);
                text::IsOk(raster))
            {
                LUDUS_LOG_INFO(LOG_CORE,
                               "text demo: rasterized glyph {} -> {}x{} coverage, bearing ({}, {})",
                               first.GlyphId,
                               bitmap.Width,
                               bitmap.Height,
                               bitmap.BearingLeft,
                               bitmap.BearingTop);
                LogCoveragePreview(bitmap);
            }
            else
            {
                LUDUS_LOG_ERROR(LOG_CORE, "text demo: RasterizeGlyph failed: {}", text::ToString(raster));
                ok = false;
            }
        }
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: GetLayout failed: {}", text::ToString(status));
        ok = false;
    }

    // 6. Tear down in reverse order: layout, then font, then the system.
    (void)text::DestroyLayout(system, layout);
    (void)text::UnloadFont(system, font);
    if (text::Status status = text::DestroyFontSystem(system); text::IsOk(status) == false)
    {
        LUDUS_LOG_ERROR(LOG_CORE, "text demo: DestroyFontSystem failed: {}", text::ToString(status));
        ok = false;
    }
    return ok;
}
} // namespace

bool RunOnce() noexcept
{
    static bool done = false;
    if (done)
    {
        return true;
    }
    done = true;
    return RunDemo();
}
} // namespace ludus::smoke::text_demo
