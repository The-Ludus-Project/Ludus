// CPU Text implementation: owned font bytes/faces and immutable UTF-8 shaped
// runs/metrics through the pinned FreeType 2.14.3 + HarfBuzz 14.5.1 sources.
// See .kiro/specs/text-font-rendering/design.md section 3.
//
// FreeType and HarfBuzz are confined to this translation unit; their types
// never appear in a public header (requirement T15). All public entry points
// are noexcept and report Status; no C++ exceptions are used (AGENTS.md).

#include <ludus/text/font_system.h>

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <hb-ft.h>
#include <hb.h>

#include "internal/raster_layout.h"
#include "internal/utf8.h"

#include <new>
#include <string>

namespace ludus::text
{
namespace
{
using ludus::foundation::uint64;
using ludus::foundation::uint8;

// --- Defaults (design section 6) -------------------------------------------
constexpr uint32 kDefaultMaxFonts = 8;
constexpr uint32 kDefaultMaxLayouts = 256;
constexpr usize kDefaultMaxFontBytesPerFont = 32ULL * 1024ULL * 1024ULL;
constexpr usize kDefaultMaxFontBytesTotal = 64ULL * 1024ULL * 1024ULL;
constexpr uint32 kDefaultMaxRunInputBytes = 16384;
constexpr uint32 kDefaultMaxGlyphsPerLayout = 4096;
constexpr uint32 kDefaultMaxRetainedGlyphs = 65536;
constexpr uint32 kMinPixelHeight = 1;
constexpr uint32 kMaxPixelHeight = 256;

// hb-ft load flags kept identical for metrics and rasterization (design 3).
constexpr int kLoadFlags = FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;

// Handle layout: low 20 bits slot index, high 12 bits generation. Generation 0
// never appears in a live handle, so a zeroed handle is always invalid.
constexpr uint32 kSlotBits = 20;
constexpr uint32 kSlotMask = (1U << kSlotBits) - 1U;
constexpr uint32 kMaxGeneration = (1U << (32U - kSlotBits)) - 1U;

[[nodiscard]] uint32 MakeHandle(uint32 slot, uint32 generation) noexcept
{
    return (generation << kSlotBits) | (slot & kSlotMask);
}
[[nodiscard]] uint32 HandleSlot(uint32 value) noexcept
{
    return value & kSlotMask;
}
[[nodiscard]] uint32 HandleGeneration(uint32 value) noexcept
{
    return value >> kSlotBits;
}

[[nodiscard]] float32 F26Dot6ToPixels(long value) noexcept
{
    return static_cast<float32>(value) / 64.0F;
}

// A loaded font: owned bytes plus the FreeType face and referenced HarfBuzz
// font built on top of them. The caller's bytes are never retained.
struct FontSlot final
{
    uint32 Generation = 0; // 0 == free slot.
    bool InUse = false;
    foundation::Array<uint8> Bytes;
    FT_Face Face = nullptr;
    hb_font_t* HbFont = nullptr;
    uint32 LiveLayouts = 0;
    uint32 CurrentPixelHeight = 0; // last size set on the face (0 == unset).
};

// An immutable shaped layout plus a copy of its source UTF-8. Storage is stable
// for the layout's lifetime; other layouts never relocate it.
struct LayoutSlot final
{
    uint32 Generation = 0;
    bool InUse = false;
    FontId Font;
    uint32 PixelHeight = 0;
    Direction TextDirection = Direction::LeftToRight;
    std::string Source;
    foundation::Array<ShapedGlyph> Glyphs;
    LayoutMetrics Metrics;
};
} // namespace

struct FontSystem final
{
    FontSystemConfig Config;
    FT_Library Library = nullptr;
    foundation::Array<FontSlot> Fonts;
    foundation::Array<LayoutSlot> Layouts;
    uint32 FontGenerationCursor = 1;
    uint32 LayoutGenerationCursor = 1;
    uint64 RetainedGlyphRecords = 0;
    uint64 ShapeCalls = 0;
    uint64 RasterizeCalls = 0;
    uint64 MissingGlyphs = 0;
    usize OwnedFontBytes = 0;
    // Rasterization scratch, valid only until the next raster call.
    foundation::Array<uint8> RasterScratch;
    uint32 RendererRefCount = 0; // set by GraphicsText; blocks destruction.
};

namespace
{
[[nodiscard]] uint32 ResolveLimit(uint32 configured, uint32 fallback) noexcept
{
    return configured != 0 ? configured : fallback;
}
[[nodiscard]] usize ResolveLimit(usize configured, usize fallback) noexcept
{
    return configured != 0 ? configured : fallback;
}

// Find a live font slot for `font`, validating its generation.
[[nodiscard]] FontSlot* ResolveFont(FontSystem* system, FontId font) noexcept
{
    if (system == nullptr || font.IsValid() == false)
    {
        return nullptr;
    }
    const uint32 slot = HandleSlot(font.Value);
    if (slot >= system->Fonts.GetSize())
    {
        return nullptr;
    }
    FontSlot& candidate = system->Fonts[slot];
    if (candidate.InUse == false || candidate.Generation != HandleGeneration(font.Value))
    {
        return nullptr;
    }
    return &candidate;
}

[[nodiscard]] LayoutSlot* ResolveLayout(FontSystem* system, LayoutId layout) noexcept
{
    if (system == nullptr || layout.IsValid() == false)
    {
        return nullptr;
    }
    const uint32 slot = HandleSlot(layout.Value);
    if (slot >= system->Layouts.GetSize())
    {
        return nullptr;
    }
    LayoutSlot& candidate = system->Layouts[slot];
    if (candidate.InUse == false || candidate.Generation != HandleGeneration(layout.Value))
    {
        return nullptr;
    }
    return &candidate;
}

// Advance a generation cursor, skipping 0 and wrapping below the max so an old
// handle can never alias a recycled slot at the same generation.
[[nodiscard]] uint32 NextGeneration(uint32& cursor) noexcept
{
    uint32 generation = cursor;
    cursor = (cursor >= kMaxGeneration) ? 1U : cursor + 1U;
    return generation;
}

// Thanks to the HarfBuzz contributors, "FreeType integration", Manual 14.5.1:
// https://github.com/harfbuzz/harfbuzz/blob/14.5.1/docs/usermanual-integration.xml
// (integration-freetype section). Adapt its face-size
// change notification and matching load flags to checked, reusable font slots.
// Research owner: docs/architecture/text-font-rendering-research.md.
// Ensure the FT face is sized to `pixelHeight` and the hb font agrees. Only
// re-sizes when the requested height differs from the last applied one, and
// always reports hb-ft the current state (design: alternating sizes must not
// contaminate results).
[[nodiscard]] Status SyncFaceSize(FontSlot& slot, uint32 pixelHeight) noexcept
{
    if (slot.CurrentPixelHeight != pixelHeight)
    {
        const FT_Error error = FT_Set_Pixel_Sizes(slot.Face, 0, pixelHeight);
        if (error != 0)
        {
            return Status::BackendFailure;
        }
        slot.CurrentPixelHeight = pixelHeight;
        hb_ft_font_changed(slot.HbFont);
    }
    hb_ft_font_set_load_flags(slot.HbFont, kLoadFlags);
    return Status::Ok;
}

// Reject strong-script scalar values inconsistent with the declared run script.
// Common/Inherited scalars (spaces, combining marks, joiners, variation
// selectors) are always allowed. This is input validation for the single-run
// contract, not the Unicode bidi algorithm.
[[nodiscard]] bool ScriptMatchesRun(uint32 codePoint, hb_script_t runScript) noexcept
{
    const hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    const hb_script_t script = hb_unicode_script(const_cast<hb_unicode_funcs_t*>(unicode), codePoint);
    if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN)
    {
        return true;
    }
    return script == runScript;
}

// Control characters rejected by the single-run API (design section 3): line
// breaks, tab, NUL, and bidi paragraph controls. ZWJ/ZWNJ and variation
// selectors are permitted because they shape inside supported runs.
[[nodiscard]] bool IsRejectedControl(uint32 codePoint) noexcept
{
    switch (codePoint)
    {
        case 0x00: // NUL
        case 0x09: // TAB
        case 0x0A: // LF
        case 0x0D: // CR
            return true;
        default:
            break;
    }
    // Bidi paragraph/embedding/override/isolate controls.
    if (codePoint == 0x200E || codePoint == 0x200F) // LRM/RLM
    {
        return true;
    }
    if (codePoint >= 0x202A && codePoint <= 0x202E) // LRE/RLE/PDF/LRO/RLO
    {
        return true;
    }
    if (codePoint >= 0x2066 && codePoint <= 0x2069) // LRI/RLI/FSI/PDI
    {
        return true;
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Status CreateFontSystem(const FontSystemConfig& config, FontSystem** outSystem) noexcept
{
    if (outSystem == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outSystem = nullptr;

    auto* system = new (std::nothrow) FontSystem();
    if (system == nullptr)
    {
        return Status::OutOfMemory;
    }

    system->Config = config;
    const FT_Error error = FT_Init_FreeType(&system->Library);
    if (error != 0)
    {
        delete system;
        return Status::BackendFailure;
    }

    const uint32 maxFonts = ResolveLimit(config.MaxFonts, kDefaultMaxFonts);
    const uint32 maxLayouts = ResolveLimit(config.MaxLayouts, kDefaultMaxLayouts);
    // Reserve metadata up front; warm paths never grow these. Allocation
    // failure releases the partially built system and publishes nothing.
    if (system->Fonts.TryResize(maxFonts) == false || system->Layouts.TryResize(maxLayouts) == false)
    {
        FT_Done_FreeType(system->Library);
        delete system;
        return Status::OutOfMemory;
    }

    *outSystem = system;
    return Status::Ok;
}

Status DestroyFontSystem(FontSystem* system) noexcept
{
    if (system == nullptr)
    {
        return Status::Ok; // idempotent
    }
    if (system->RendererRefCount != 0)
    {
        return Status::Busy;
    }
    for (LayoutSlot& layout : system->Layouts)
    {
        layout = LayoutSlot{};
    }
    for (FontSlot& font : system->Fonts)
    {
        if (font.InUse)
        {
            if (font.HbFont != nullptr)
            {
                hb_font_destroy(font.HbFont);
            }
            if (font.Face != nullptr)
            {
                FT_Done_Face(font.Face);
            }
        }
        font = FontSlot{};
    }
    if (system->Library != nullptr)
    {
        FT_Done_FreeType(system->Library);
    }
    delete system;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
Status LoadFont(FontSystem* system, std::span<const uint8> bytes, uint32 faceIndex, FontId* outFont) noexcept
{
    if (outFont == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outFont = FontId{};
    if (system == nullptr || bytes.empty())
    {
        return Status::InvalidArgument;
    }

    const usize perFont = ResolveLimit(system->Config.MaxFontBytesPerFont, kDefaultMaxFontBytesPerFont);
    const usize totalCap = ResolveLimit(system->Config.MaxFontBytesTotal, kDefaultMaxFontBytesTotal);
    if (bytes.size() > perFont)
    {
        return Status::ResourceLimit;
    }
    if (system->OwnedFontBytes + bytes.size() > totalCap)
    {
        return Status::ResourceLimit;
    }

    // Find a free slot.
    uint32 slotIndex = 0;
    bool found = false;
    for (uint32 i = 0; i < system->Fonts.GetSize(); ++i)
    {
        if (system->Fonts[i].InUse == false)
        {
            slotIndex = i;
            found = true;
            break;
        }
    }
    if (found == false)
    {
        return Status::ResourceLimit;
    }

    FontSlot& slot = system->Fonts[slotIndex];
    // Copy bytes into bounded owned storage before any FT/HB object references
    // them (never store a raw caller pointer).
    // Allocation failure leaves the system, slot and output untouched.
    foundation::Array<uint8> owned;
    if (owned.TryAddRange(bytes) == false)
    {
        return Status::OutOfMemory;
    }

    FT_Face face = nullptr;
    const FT_Error error = FT_New_Memory_Face(system->Library,
                                              reinterpret_cast<const FT_Byte*>(owned.GetData()),
                                              static_cast<FT_Long>(owned.GetSize()),
                                              static_cast<FT_Long>(faceIndex),
                                              &face);
    if (error != 0)
    {
        return Status::UnsupportedFont;
    }
    // Require a scalable outline face (TTF/OTF). Bitmap-only faces are out of
    // scope for this milestone.
    if ((face->face_flags & FT_FACE_FLAG_SCALABLE) == 0)
    {
        FT_Done_Face(face);
        return Status::UnsupportedFont;
    }

    hb_font_t* hbFont = hb_ft_font_create_referenced(face);
    if (hbFont == nullptr)
    {
        FT_Done_Face(face);
        return Status::OutOfMemory;
    }

    slot.Generation = NextGeneration(system->FontGenerationCursor);
    slot.InUse = true;
    slot.Bytes = std::move(owned);
    slot.Face = face;
    slot.HbFont = hbFont;
    slot.LiveLayouts = 0;
    slot.CurrentPixelHeight = 0;
    system->OwnedFontBytes += slot.Bytes.GetSize();

    *outFont = FontId{MakeHandle(slotIndex, slot.Generation)};
    return Status::Ok;
}

Status UnloadFont(FontSystem* system, FontId font) noexcept
{
    FontSlot* slot = ResolveFont(system, font);
    if (slot == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (slot->LiveLayouts != 0)
    {
        return Status::Busy;
    }
    // Release hb/FT objects before the owned bytes they reference.
    if (slot->HbFont != nullptr)
    {
        hb_font_destroy(slot->HbFont);
    }
    if (slot->Face != nullptr)
    {
        FT_Done_Face(slot->Face);
    }
    system->OwnedFontBytes -= slot->Bytes.GetSize();
    *slot = FontSlot{};
    return Status::Ok;
}

Status GetGlyphCount(FontSystem* system, FontId font, uint32* outCount) noexcept
{
    if (outCount == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outCount = 0;
    FontSlot* slot = ResolveFont(system, font);
    if (slot == nullptr)
    {
        return Status::InvalidHandle;
    }
    *outCount = static_cast<uint32>(slot->Face->num_glyphs);
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Shaping
// ---------------------------------------------------------------------------
namespace
{
Status ShapeInternal(FontSystem* system,
                     FontSlot& fontSlot,
                     FontId font,
                     std::string_view utf8,
                     const RunSpec& runSpec,
                     LayoutId* outLayout) noexcept
{
    const uint32 maxInput = ResolveLimit(system->Config.MaxRunInputBytes, kDefaultMaxRunInputBytes);
    const uint32 maxGlyphs = ResolveLimit(system->Config.MaxGlyphsPerLayout, kDefaultMaxGlyphsPerLayout);
    const uint32 maxRetained = ResolveLimit(system->Config.MaxRetainedGlyphs, kDefaultMaxRetainedGlyphs);

    if (runSpec.PixelHeight < kMinPixelHeight || runSpec.PixelHeight > kMaxPixelHeight)
    {
        return Status::InvalidArgument;
    }
    if (utf8.size() > maxInput)
    {
        return Status::ResourceLimit;
    }

    // Strict UTF-8 validation first.
    const internal::Utf8Result validation = internal::ValidateUtf8(utf8);
    if (validation.Valid == false)
    {
        return Status::InvalidUtf8;
    }

    const hb_script_t runScript =
        hb_script_from_iso15924_tag(HB_TAG(runSpec.Script[0], runSpec.Script[1], runSpec.Script[2], runSpec.Script[3]));

    // Control/script validation over all scalars.
    for (usize offset = 0; offset < utf8.size();)
    {
        uint32 codePoint = 0;
        const usize consumed = internal::DecodeUtf8(utf8, offset, &codePoint);
        offset += consumed;
        if (IsRejectedControl(codePoint))
        {
            return Status::UnsupportedText;
        }
        if (ScriptMatchesRun(codePoint, runScript) == false)
        {
            return Status::UnsupportedText;
        }
    }

    // Find a free layout slot before shaping so a late failure publishes nothing.
    uint32 layoutIndex = 0;
    bool found = false;
    for (uint32 i = 0; i < system->Layouts.GetSize(); ++i)
    {
        if (system->Layouts[i].InUse == false)
        {
            layoutIndex = i;
            found = true;
            break;
        }
    }
    if (found == false)
    {
        return Status::ResourceLimit;
    }

    const Status sizeStatus = SyncFaceSize(fontSlot, runSpec.PixelHeight);
    if (IsOk(sizeStatus) == false)
    {
        return sizeStatus;
    }

    hb_buffer_t* buffer = hb_buffer_create();
    if (buffer == nullptr || hb_buffer_allocation_successful(buffer) == 0)
    {
        if (buffer != nullptr)
        {
            hb_buffer_destroy(buffer);
        }
        return Status::OutOfMemory;
    }

    // Thanks to the HarfBuzz contributors, "Working with HarfBuzz clusters",
    // HarfBuzz Manual 14.5.1, cluster levels 0 and 1:
    // https://github.com/harfbuzz/harfbuzz/blob/14.5.1/docs/usermanual-clusters.xml .
    // Use input UTF-8 byte offsets as cluster values. Level 0 may merge mark/
    // joiner clusters with the preceding base and retains monotone associations
    // through shaping/reordering; individual input boundaries need not survive.
    // This is not a full paragraph bidi/caret model. Research owner:
    // docs/architecture/text-font-rendering-research.md.
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_direction(buffer,
                            runSpec.TextDirection == Direction::RightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, runScript);
    hb_buffer_set_language(buffer, hb_language_from_string(runSpec.Language, -1));

    hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    if (hb_buffer_allocation_successful(buffer) == 0)
    {
        hb_buffer_destroy(buffer);
        return Status::OutOfMemory;
    }

    hb_shape(fontSlot.HbFont, buffer, nullptr, 0);
    if (hb_buffer_allocation_successful(buffer) == 0)
    {
        hb_buffer_destroy(buffer);
        return Status::OutOfMemory;
    }

    unsigned int glyphCount = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &glyphCount);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);

    if (glyphCount > maxGlyphs)
    {
        hb_buffer_destroy(buffer);
        return Status::ResourceLimit;
    }
    if (system->RetainedGlyphRecords + glyphCount > maxRetained)
    {
        hb_buffer_destroy(buffer);
        return Status::ResourceLimit;
    }

    // Build the immutable layout. Keep HarfBuzz's returned order (RTL included);
    // never reverse UTF-8 bytes or re-add kerning.
    LayoutSlot staging;
    // Reserve every record before the loop so appending below cannot allocate.
    if (staging.Glyphs.TryEnsureCapacity(glyphCount) == false)
    {
        hb_buffer_destroy(buffer);
        return Status::OutOfMemory;
    }
    staging.Source.assign(utf8.data(), utf8.size());

    // Pen-walked ink bounds and advance, in physical pixels.
    float32 penX = 0.0F;
    float32 penY = 0.0F;
    float32 inkLeft = 0.0F;
    float32 inkTop = 0.0F;
    float32 inkRight = 0.0F;
    float32 inkBottom = 0.0F;
    bool haveInk = false;
    uint32 missing = 0;

    for (unsigned int i = 0; i < glyphCount; ++i)
    {
        ShapedGlyph glyph;
        glyph.GlyphId = infos[i].codepoint; // post-shaping this is a glyph index
        glyph.Cluster = infos[i].cluster;
        glyph.XAdvance = static_cast<int32>(positions[i].x_advance);
        glyph.YAdvance = static_cast<int32>(positions[i].y_advance);
        glyph.XOffset = static_cast<int32>(positions[i].x_offset);
        glyph.YOffset = static_cast<int32>(positions[i].y_offset);
        glyph.IsMissing = glyph.GlyphId == 0;
        if (glyph.IsMissing)
        {
            ++missing;
        }

        // Ink extent from glyph bbox at the current pen (via FreeType).
        if (FT_Load_Glyph(fontSlot.Face, glyph.GlyphId, kLoadFlags) == 0)
        {
            const FT_GlyphSlot gs = fontSlot.Face->glyph;
            const float32 gx = penX + F26Dot6ToPixels(positions[i].x_offset) + static_cast<float32>(gs->bitmap_left);
            const float32 gyTop = penY - F26Dot6ToPixels(positions[i].y_offset) - static_cast<float32>(gs->bitmap_top);
            const float32 metricsW = F26Dot6ToPixels(static_cast<long>(gs->metrics.width));
            const float32 metricsH = F26Dot6ToPixels(static_cast<long>(gs->metrics.height));
            if (metricsW > 0.0F && metricsH > 0.0F)
            {
                const float32 left = gx;
                const float32 top = gyTop;
                const float32 right = gx + metricsW;
                const float32 bottom = gyTop + metricsH;
                if (haveInk == false)
                {
                    inkLeft = left;
                    inkTop = top;
                    inkRight = right;
                    inkBottom = bottom;
                    haveInk = true;
                }
                else
                {
                    inkLeft = left < inkLeft ? left : inkLeft;
                    inkTop = top < inkTop ? top : inkTop;
                    inkRight = right > inkRight ? right : inkRight;
                    inkBottom = bottom > inkBottom ? bottom : inkBottom;
                }
            }
        }

        penX += F26Dot6ToPixels(positions[i].x_advance);
        penY += F26Dot6ToPixels(positions[i].y_advance);
        staging.Glyphs.Add(glyph);
    }

    hb_buffer_destroy(buffer);

    // Face-level metrics (sized).
    const FT_Size_Metrics& sm = fontSlot.Face->size->metrics;
    staging.Metrics.Ascent = F26Dot6ToPixels(sm.ascender);
    staging.Metrics.Descent = -F26Dot6ToPixels(sm.descender); // descender is negative
    staging.Metrics.LineAdvance = F26Dot6ToPixels(sm.height);
    staging.Metrics.AdvanceX = penX;
    staging.Metrics.AdvanceY = penY;
    staging.Metrics.GlyphCount = glyphCount;
    staging.Metrics.MissingGlyphCount = missing;
    if (haveInk)
    {
        staging.Metrics.Ink = InkBounds{inkLeft, inkTop, inkRight, inkBottom};
    }

    // Commit the slot only now that everything succeeded.
    LayoutSlot& slot = system->Layouts[layoutIndex];
    slot.Generation = NextGeneration(system->LayoutGenerationCursor);
    slot.InUse = true;
    slot.Font = font;
    slot.PixelHeight = runSpec.PixelHeight;
    slot.TextDirection = runSpec.TextDirection;
    slot.Source = std::move(staging.Source);
    slot.Glyphs = std::move(staging.Glyphs);
    slot.Metrics = staging.Metrics;

    ++fontSlot.LiveLayouts;
    system->RetainedGlyphRecords += glyphCount;
    system->MissingGlyphs += missing;
    ++system->ShapeCalls;

    *outLayout = LayoutId{MakeHandle(layoutIndex, slot.Generation)};
    return Status::Ok;
}
} // namespace

Status
ShapeRun(FontSystem* system, FontId font, std::string_view utf8, const RunSpec& runSpec, LayoutId* outLayout) noexcept
{
    if (outLayout == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outLayout = LayoutId{};
    FontSlot* fontSlot = ResolveFont(system, font);
    if (fontSlot == nullptr)
    {
        return Status::InvalidHandle;
    }
    return ShapeInternal(system, *fontSlot, font, utf8, runSpec, outLayout);
}

Status
ShapeLatin(FontSystem* system, FontId font, std::string_view utf8, uint32 pixelHeight, LayoutId* outLayout) noexcept
{
    RunSpec spec;
    spec.PixelHeight = pixelHeight;
    spec.TextDirection = Direction::LeftToRight;
    spec.Script[0] = 'L';
    spec.Script[1] = 'a';
    spec.Script[2] = 't';
    spec.Script[3] = 'n';
    spec.Language[0] = 'u';
    spec.Language[1] = 'n';
    spec.Language[2] = 'd';
    spec.Language[3] = '\0';
    return ShapeRun(system, font, utf8, spec, outLayout);
}

Status GetLayout(FontSystem* system, LayoutId layout, LayoutView* outView) noexcept
{
    if (outView == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outView = LayoutView{};
    LayoutSlot* slot = ResolveLayout(system, layout);
    if (slot == nullptr)
    {
        return Status::InvalidHandle;
    }
    outView->Font = slot->Font;
    outView->PixelHeight = slot->PixelHeight;
    outView->TextDirection = slot->TextDirection;
    outView->Glyphs = slot->Glyphs.AsSpan();
    outView->SourceUtf8 = std::string_view(slot->Source.data(), slot->Source.size());
    outView->Metrics = slot->Metrics;
    return Status::Ok;
}

Status DestroyLayout(FontSystem* system, LayoutId layout) noexcept
{
    LayoutSlot* slot = ResolveLayout(system, layout);
    if (slot == nullptr)
    {
        return Status::InvalidHandle;
    }
    FontSlot* fontSlot = ResolveFont(system, slot->Font);
    if (fontSlot != nullptr && fontSlot->LiveLayouts != 0)
    {
        --fontSlot->LiveLayouts;
    }
    if (system->RetainedGlyphRecords >= slot->Glyphs.GetSize())
    {
        system->RetainedGlyphRecords -= slot->Glyphs.GetSize();
    }
    *slot = LayoutSlot{};
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Cold rasterization
// ---------------------------------------------------------------------------
// pixelHeight and glyphId are distinct domains of the stable public cold-raster
// signature; validated independently below and documented in the header.
Status RasterizeGlyph(FontSystem* system,
                      FontId font,
                      uint32 pixelHeight, // NOLINT(bugprone-easily-swappable-parameters)
                      uint32 glyphId,
                      GlyphBitmap* outBitmap) noexcept
{
    if (outBitmap == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outBitmap = GlyphBitmap{};
    if (pixelHeight < kMinPixelHeight || pixelHeight > kMaxPixelHeight)
    {
        return Status::InvalidArgument;
    }
    FontSlot* slot = ResolveFont(system, font);
    if (slot == nullptr)
    {
        return Status::InvalidHandle;
    }

    const Status sizeStatus = SyncFaceSize(*slot, pixelHeight);
    if (IsOk(sizeStatus) == false)
    {
        return sizeStatus;
    }

    if (FT_Load_Glyph(slot->Face, glyphId, kLoadFlags) != 0)
    {
        return Status::BackendFailure;
    }
    FT_GlyphSlot gs = slot->Face->glyph;

    // Reject color/BGRA/LCD formats before treating anything as R8 coverage.
    if (gs->format != FT_GLYPH_FORMAT_OUTLINE && gs->format != FT_GLYPH_FORMAT_BITMAP)
    {
        return Status::UnsupportedGlyphFormat;
    }

    if (FT_Render_Glyph(gs, FT_RENDER_MODE_NORMAL) != 0)
    {
        return Status::BackendFailure;
    }
    const FT_Bitmap& bmp = gs->bitmap;

    // Only 8-bit grayscale coverage is accepted.
    if (bmp.pixel_mode != FT_PIXEL_MODE_GRAY)
    {
        return Status::UnsupportedGlyphFormat;
    }
    if (bmp.num_grays != 0 && bmp.num_grays != 256)
    {
        // Non-256 gray range: reject explicitly rather than mis-scale.
        return Status::UnsupportedGlyphFormat;
    }

    // Zero-area glyph (e.g. space): valid metrics, no coverage, no allocation.
    if (bmp.width == 0 || bmp.rows == 0)
    {
        ++system->RasterizeCalls;
        outBitmap->GlyphId = glyphId;
        outBitmap->BearingLeft = gs->bitmap_left;
        outBitmap->BearingTop = gs->bitmap_top;
        outBitmap->Width = 0;
        outBitmap->Height = 0;
        system->RasterScratch.Clear();
        outBitmap->Coverage = std::span<const uint8>();
        return Status::Ok;
    }

    const uint32 width = bmp.width;
    const uint32 height = bmp.rows;
    internal::RasterLayout layout;
    const auto layoutStatus = internal::TryRasterLayout(
        {
            .Width = width,
            .Height = height,
            .Pitch = bmp.pitch,
        },
        layout);
    if (layoutStatus != Status::Ok)
    {
        return layoutStatus;
    }
    if (bmp.buffer == nullptr)
    {
        return Status::BackendFailure;
    }
    if (layout.CoverageBytes > foundation::Array<uint8>::GetMaxSize())
    {
        return Status::ResourceLimit;
    }
    const usize absPitch = layout.Pitch;

    // Normalize into top-down, tightly packed coverage (Width is not a stride).
    const usize total = layout.CoverageBytes;
    if (system->RasterScratch.TryResize(total) == false)
    {
        return Status::OutOfMemory;
    }
    for (uint32 row = 0; row < height; ++row)
    {
        // When pitch < 0 the buffer is bottom-up; read rows in reverse.
        const uint8* srcRow = (bmp.pitch >= 0) ? (bmp.buffer + static_cast<usize>(row) * absPitch)
                                               : (bmp.buffer + static_cast<usize>(height - 1U - row) * absPitch);
        uint8* dstRow = system->RasterScratch.GetData() + static_cast<usize>(row) * width;
        for (uint32 col = 0; col < width; ++col)
        {
            dstRow[col] = srcRow[col];
        }
    }

    // Publish only after the scratch allocation succeeded; failure above leaves
    // the cleared output and the counters unchanged.
    ++system->RasterizeCalls;
    outBitmap->GlyphId = glyphId;
    outBitmap->BearingLeft = gs->bitmap_left;
    outBitmap->BearingTop = gs->bitmap_top;
    outBitmap->Width = width;
    outBitmap->Height = height;
    outBitmap->Coverage = system->RasterScratch.AsSpan();
    return Status::Ok;
}

FontSystemCounters GetCounters(FontSystem* system) noexcept
{
    FontSystemCounters counters;
    if (system == nullptr)
    {
        return counters;
    }
    uint32 liveFonts = 0;
    uint32 liveLayouts = 0;
    for (const FontSlot& font : system->Fonts)
    {
        if (font.InUse)
        {
            ++liveFonts;
        }
    }
    for (const LayoutSlot& layout : system->Layouts)
    {
        if (layout.InUse)
        {
            ++liveLayouts;
        }
    }
    counters.LoadedFonts = liveFonts;
    counters.LiveLayouts = liveLayouts;
    counters.RetainedGlyphRecords = system->RetainedGlyphRecords;
    counters.ShapeCalls = system->ShapeCalls;
    counters.RasterizeCalls = system->RasterizeCalls;
    counters.MissingGlyphs = system->MissingGlyphs;
    counters.OwnedFontBytes = system->OwnedFontBytes;
    return counters;
}
} // namespace ludus::text
