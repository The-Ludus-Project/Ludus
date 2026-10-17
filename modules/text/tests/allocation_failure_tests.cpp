// Allocation-failure tests for Text-owned storage (container-systems M1). The
// aligned nothrow operator new is the Ludus Array allocation seam; FreeType and
// HarfBuzz allocate through malloc and FontSystem itself through the unaligned
// nothrow new, so rejecting only the aligned nothrow overload fails exactly the
// Array allocations. Built as its own executable, without sanitizers, because
// it replaces global allocation functions.

#include <ludus/text/font_system.h>

#include "fixture_support.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
bool gShouldRejectArrayAllocation = false;
ludus::foundation::uint64 gRejectedAllocations = 0;

void* AllocateAligned(std::size_t bytes, std::size_t alignment) noexcept
{
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    std::size_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
    if (rounded == 0)
    {
        rounded = alignment;
    }
    return std::aligned_alloc(alignment, rounded);
}

// Arms rejection for one scope so Catch2 assertions run with allocation intact.
struct RejectArrayAllocationScope final
{
    RejectArrayAllocationScope() noexcept
    {
        gRejectedAllocations = 0;
        gShouldRejectArrayAllocation = true;
    }
    ~RejectArrayAllocationScope() noexcept
    {
        gShouldRejectArrayAllocation = false;
    }
    RejectArrayAllocationScope(const RejectArrayAllocationScope&) = delete;
    RejectArrayAllocationScope& operator=(const RejectArrayAllocationScope&) = delete;
};
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    if (gShouldRejectArrayAllocation == true)
    {
        ++gRejectedAllocations;
        return nullptr;
    }
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::align_val_t) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    std::free(pointer);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::text;
using ludus::foundation::Array;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::text::test::LatinFont;
using ludus::text::test::LoadFixtureFont;

namespace
{
struct SystemGuard final
{
    FontSystem* SystemOrNull = nullptr;
    SystemGuard()
    {
        REQUIRE(IsOk(CreateFontSystem(FontSystemConfig{}, &SystemOrNull)));
    }
    ~SystemGuard()
    {
        (void)DestroyFontSystem(SystemOrNull);
    }
    SystemGuard(const SystemGuard&) = delete;
    SystemGuard& operator=(const SystemGuard&) = delete;
};

[[nodiscard]] bool AreCountersEqual(const FontSystemCounters& left, const FontSystemCounters& right) noexcept
{
    return left.LoadedFonts == right.LoadedFonts && left.LiveLayouts == right.LiveLayouts &&
           left.RetainedGlyphRecords == right.RetainedGlyphRecords && left.ShapeCalls == right.ShapeCalls &&
           left.RasterizeCalls == right.RasterizeCalls && left.MissingGlyphs == right.MissingGlyphs &&
           left.OwnedFontBytes == right.OwnedFontBytes;
}
} // namespace

TEST_CASE("Font system creation reports metadata allocation failure", "[text][alloc]")
{
    // Start from a non-null sentinel so the test observes the published null.
    alignas(std::max_align_t) static uint8 sentinel = 0;
    FontSystem* systemOrNull = reinterpret_cast<FontSystem*>(&sentinel);
    Status status = Status::Ok;
    {
        RejectArrayAllocationScope reject;
        status = CreateFontSystem(FontSystemConfig{}, &systemOrNull);
    }
    CHECK(gRejectedAllocations > 0);
    CHECK(status == Status::OutOfMemory);
    CHECK(systemOrNull == nullptr);
}

TEST_CASE("Font loading reports byte-copy allocation failure without changing the system", "[text][alloc]")
{
    SystemGuard guard;
    const Array<uint8> bytes = LoadFixtureFont(LatinFont());
    REQUIRE(bytes.IsEmpty() == false);
    const FontSystemCounters before = GetCounters(guard.SystemOrNull);

    // A stale-looking non-zero ID shows that failure clears the output.
    FontId font{0x00100000U};
    Status status = Status::Ok;
    {
        RejectArrayAllocationScope reject;
        status = LoadFont(guard.SystemOrNull, bytes, 0, &font);
    }
    CHECK(gRejectedAllocations > 0);
    CHECK(status == Status::OutOfMemory);
    CHECK(font.IsValid() == false);
    CHECK(AreCountersEqual(GetCounters(guard.SystemOrNull), before));

    // The rejected attempt consumed no slot: a later load still succeeds.
    REQUIRE(IsOk(LoadFont(guard.SystemOrNull, bytes, 0, &font)));
    CHECK(font.IsValid());
}

TEST_CASE("Shaping reports glyph-record allocation failure without publishing a layout", "[text][alloc]")
{
    SystemGuard guard;
    const Array<uint8> bytes = LoadFixtureFont(LatinFont());
    REQUIRE(bytes.IsEmpty() == false);
    FontId font;
    REQUIRE(IsOk(LoadFont(guard.SystemOrNull, bytes, 0, &font)));
    const FontSystemCounters before = GetCounters(guard.SystemOrNull);

    LayoutId layout;
    Status status = Status::Ok;
    {
        RejectArrayAllocationScope reject;
        status = ShapeLatin(guard.SystemOrNull, font, "Ludus", 32, &layout);
    }
    CHECK(gRejectedAllocations > 0);
    CHECK(status == Status::OutOfMemory);
    CHECK(layout.IsValid() == false);
    CHECK(AreCountersEqual(GetCounters(guard.SystemOrNull), before));

    // Font references were not leaked: the font can still be unloaded.
    REQUIRE(IsOk(ShapeLatin(guard.SystemOrNull, font, "Ludus", 32, &layout)));
    REQUIRE(IsOk(DestroyLayout(guard.SystemOrNull, layout)));
    CHECK(IsOk(UnloadFont(guard.SystemOrNull, font)));
}

TEST_CASE("Rasterization reports scratch allocation failure with a cleared bitmap", "[text][alloc]")
{
    SystemGuard guard;
    const Array<uint8> bytes = LoadFixtureFont(LatinFont());
    REQUIRE(bytes.IsEmpty() == false);
    FontId font;
    REQUIRE(IsOk(LoadFont(guard.SystemOrNull, bytes, 0, &font)));
    LayoutId layout;
    REQUIRE(IsOk(ShapeLatin(guard.SystemOrNull, font, "A", 48, &layout)));
    LayoutView view;
    REQUIRE(IsOk(GetLayout(guard.SystemOrNull, layout, &view)));
    REQUIRE(view.Glyphs.size() == 1);
    const uint32 glyph = view.Glyphs[0].GlyphId;
    const FontSystemCounters before = GetCounters(guard.SystemOrNull);

    GlyphBitmap bitmap;
    Status status = Status::Ok;
    {
        RejectArrayAllocationScope reject;
        status = RasterizeGlyph(guard.SystemOrNull, font, 48, glyph, &bitmap);
    }
    CHECK(gRejectedAllocations > 0);
    CHECK(status == Status::OutOfMemory);
    CHECK(bitmap.Width == 0);
    CHECK(bitmap.Height == 0);
    CHECK(bitmap.Coverage.empty());
    CHECK(AreCountersEqual(GetCounters(guard.SystemOrNull), before));

    REQUIRE(IsOk(RasterizeGlyph(guard.SystemOrNull, font, 48, glyph, &bitmap)));
    CHECK(bitmap.Coverage.empty() == false);
}
