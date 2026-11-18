#pragma once

// Public CPU text contract for Ludus (.kiro/specs/text-font-rendering/design.md
// section 3). The FontSystem owns font bytes/faces and immutable UTF-8 shaped
// runs/metrics produced through the pinned HarfBuzz + FreeType sources.
//
// Ownership and lifetime (design section 3, requirement T03):
//   * LoadFont copies the caller's bytes into bounded owned storage. The caller
//     span is never retained.
//   * FontId / LayoutId are generation-checked handles; a stale handle fails
//     with Status::InvalidHandle rather than aliasing a recycled slot.
//   * ShapeRun publishes only a complete, immutable layout; empty input is a
//     valid empty layout.
//
// Scope: a run has exactly one font, physical pixel size, script, language, and
// explicit horizontal direction. This is a shaped-run API, not a paragraph or
// bidi engine: no automatic itemization, fallback, wrapping, or editing.
//
// This header is lightweight: no FreeType/HarfBuzz types, no heavy STL. It uses
// only the fixed-width types, <string_view>, and <span>. The implementation and
// all third-party headers stay private.

#include <ludus/foundation/base/types.h>

#include <ludus/text/status.h>

#include <span>
#include <string_view>

namespace ludus::text
{
using ludus::foundation::float32;
using ludus::foundation::int32;
using ludus::foundation::uint16;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

// Opaque owner. One FontSystem is used by a single owning application thread;
// there is no internal locking or process-global font cache.
struct FontSystem;

// Generation-checked handles. Slot index (low bits) plus a nonzero generation
// (high bits); a zero handle is always invalid. Generation wrap retires a slot
// so an old handle can never become valid again.
struct FontId final
{
    uint32 Value = 0;
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Value != 0;
    }
    [[nodiscard]] friend constexpr bool operator==(FontId, FontId) noexcept = default;
};

struct LayoutId final
{
    uint32 Value = 0;
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return Value != 0;
    }
    [[nodiscard]] friend constexpr bool operator==(LayoutId, LayoutId) noexcept = default;
};

enum class Direction : ludus::foundation::uint8
{
    LeftToRight,
    RightToLeft,
};

/// @brief A run's declared properties.
///
/// Script is a 4-char ISO 15924 tag (e.g. "Latn", "Hang", "Arab"); Language is a bounded BCP-47/ISO tag defaulting
/// to "und".
/// PixelHeight is the integer physical EM pixel height (1..256).
struct RunSpec final
{
    uint32 PixelHeight = 16;
    Direction TextDirection = Direction::LeftToRight;
    /// Exactly four bytes of ISO 15924 script tag; unused trailing bytes are ' '.
    char Script[4] = {'L', 'a', 't', 'n'};
    /// Bounded language tag storage; NUL-terminated, truncated past the bound.
    char Language[16] = {'u', 'n', 'd', '\0'};
};

/// @brief One shaped glyph.
///
/// Positions are signed 26.6 fixed-point physical pixel units as returned by HarfBuzz; the byte cluster is the
/// first original UTF-8 byte offset that produced this glyph (diagnostic provenance, not a caret API).
struct ShapedGlyph final
{
    /// Number of shaped advance or offset units in one physical framebuffer pixel.
    static constexpr int32 UNITS_PER_PIXEL = 64;
    uint32 GlyphId = 0;     ///< FreeType/OpenType glyph index (0 == .notdef).
    uint32 Cluster = 0;     ///< UTF-8 byte offset of the originating cluster.
    int32 XAdvance = 0;     ///< 26.6 signed.
    int32 YAdvance = 0;     ///< 26.6 signed.
    int32 XOffset = 0;      ///< 26.6 signed.
    int32 YOffset = 0;      ///< 26.6 signed.
    bool IsMissing = false; ///< True when GlyphId resolved to .notdef.
};

/// @brief Baseline-relative ink bounds in physical pixels (unsnapped).
///
/// Left/Top may be negative (overhang / negative bearing). An empty layout has a zero rectangle.
struct InkBounds final
{
    float32 Left = 0;
    float32 Top = 0;
    float32 Right = 0;
    float32 Bottom = 0;

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return Right <= Left || Bottom <= Top;
    }
};

/// @brief Measured metrics for a layout, in physical pixels.
///
/// Advance is the pen delta; Ascent/Descent are positive screen distances above/below the baseline; LineAdvance is
/// the sized face's line spacing (distinct from the ink height).
struct LayoutMetrics final
{
    float32 AdvanceX = 0;
    float32 AdvanceY = 0;
    float32 Ascent = 0;
    float32 Descent = 0;
    float32 LineAdvance = 0;
    InkBounds Ink;
    uint32 GlyphCount = 0;
    uint32 MissingGlyphCount = 0;
};

/// @brief Borrowed, read-only view over a layout's immutable storage.
///
/// Valid until that layout or the owning FontSystem is destroyed; creating or destroying other layouts never
/// relocates this storage.
struct LayoutView final
{
    FontId Font;
    uint32 PixelHeight = 0;
    Direction TextDirection = Direction::LeftToRight;
    std::span<const ShapedGlyph> Glyphs;
    std::string_view SourceUtf8;
    LayoutMetrics Metrics;
};

/// @brief Rasterized glyph coverage.
///
/// The bitmap is top-down, tightly packed (Width is not a stride) unsigned R8 coverage normalized to 0..255. Zero-
/// area glyphs (e.g. space) report valid metrics with Width==Height==0 and empty Coverage.
struct GlyphBitmap final
{
    uint32 GlyphId = 0;
    uint32 Width = 0;                                   ///< texels
    uint32 Height = 0;                                  ///< texels
    int32 BearingLeft = 0;                              ///< integer pixels, pen-relative
    int32 BearingTop = 0;                               ///< integer pixels, pen-relative (above baseline +)
    std::span<const ludus::foundation::uint8> Coverage; ///< size == Width*Height
};

/// @brief Configuration/budgets for a FontSystem (design section 6 defaults).
///
/// Zero fields select the documented default.
struct FontSystemConfig final
{
    uint32 MaxFonts = 0;           // default 8
    uint32 MaxLayouts = 0;         // default 256
    usize MaxFontBytesPerFont = 0; // default 32 MiB
    usize MaxFontBytesTotal = 0;   // default 64 MiB
    uint32 MaxRunInputBytes = 0;   // default 16384
    uint32 MaxGlyphsPerLayout = 0; // default 4096
    uint32 MaxRetainedGlyphs = 0;  // default 65536
};

// --- Lifecycle -------------------------------------------------------------
/// @brief Creates an owned context with all font/layout slot metadata preallocated.
///
/// On failure, including OutOfMemory for that metadata, publishes no owner (outSystem = nullptr).
[[nodiscard]] Status CreateFontSystem(const FontSystemConfig& config, FontSystem** outSystem) noexcept;
/// @brief Returns Busy if a renderer still references the system.
///
/// Safe after partial creation and idempotent for a null pointer.
[[nodiscard]] Status DestroyFontSystem(FontSystem* system) noexcept;

// --- Fonts -----------------------------------------------------------------
/// @brief Copies bounded bytes and builds a sized-capable outline face plus referenced HarfBuzz font. faceIndex
/// selects a face within a collection.
///
/// On failure the output ID is invalid. OutOfMemory (owned byte copy) leaves the system, its counters and every
/// loaded font unchanged.
[[nodiscard]] Status LoadFont(FontSystem* system,
                              std::span<const ludus::foundation::uint8> bytes,
                              uint32 faceIndex,
                              FontId* outFont) noexcept;
/// @brief Checks generation and live layouts; returns Busy if referenced, otherwise
/// releases hb/FT objects before the owned bytes.
[[nodiscard]] Status UnloadFont(FontSystem* system, FontId font) noexcept;
/// Number of outline glyphs in the face (for .notdef/bounds diagnostics).
[[nodiscard]] Status GetGlyphCount(FontSystem* system, FontId font, uint32* outCount) noexcept;

// --- Shaping / layouts -----------------------------------------------------
/// @brief Validates UTF-8 and the run spec, shapes through hb-ft, and owns an immutable layout plus a copy of its
/// source UTF-8.
///
/// Empty input is a valid empty layout. A failure, including OutOfMemory for the glyph records, publishes no layout
/// and leaves counters and existing layouts unchanged.
[[nodiscard]] Status
ShapeRun(FontSystem* system, FontId font, std::string_view utf8, const RunSpec& runSpec, LayoutId* outLayout) noexcept;
/// @brief Convenience Latin-LTR entry point (script "Latn", language "und").
///
/// Never pretends to handle a mixed-script paragraph.
[[nodiscard]] Status
ShapeLatin(FontSystem* system, FontId font, std::string_view utf8, uint32 pixelHeight, LayoutId* outLayout) noexcept;
/// Borrows glyph/metric/source views; valid until the layout or system is gone.
[[nodiscard]] Status GetLayout(FontSystem* system, LayoutId layout, LayoutView* outView) noexcept;
/// Releases owned storage and the font reference; stale IDs return InvalidHandle.
[[nodiscard]] Status DestroyLayout(FontSystem* system, LayoutId layout) noexcept;

// --- Cold rasterization ----------------------------------------------------
/// @brief Explicit cold operation.
///
/// The borrowed normalized coverage in outBitmap is valid only until the next raster call or system destruction;
/// the caller must copy it immediately. Restores the requested size/load state before rendering. On failure,
/// including OutOfMemory for the scratch coverage, outBitmap stays cleared and RasterizeCalls is not incremented.
[[nodiscard]] Status
RasterizeGlyph(FontSystem* system, FontId font, uint32 pixelHeight, uint32 glyphId, GlyphBitmap* outBitmap) noexcept;

// --- Bounded diagnostics ---------------------------------------------------
struct FontSystemCounters final
{
    uint32 LoadedFonts = 0;
    uint32 LiveLayouts = 0;
    uint64 RetainedGlyphRecords = 0;
    uint64 ShapeCalls = 0;
    uint64 RasterizeCalls = 0;
    uint64 MissingGlyphs = 0;
    usize OwnedFontBytes = 0;
};
[[nodiscard]] FontSystemCounters GetCounters(FontSystem* system) noexcept;
} // namespace ludus::text
