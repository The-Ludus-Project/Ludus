#include "internal/raster_layout.h"

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::text::internal
{
// Thanks to the FreeType Project, FreeType API Reference, "FT_Bitmap", for the
// signed pitch/padded-row contract:
// https://freetype.org/freetype2/docs/reference/ft2-basic_types.html#ft_bitmap
// Thanks to Eric Lengyel, "Bit Hacks for Games", Game Engine Gems 2, chapter 24,
// section 24.1, for the signed-minimum counterexample. We widen before negating
// and check byte products/extents; no bit-hack listing is copied.
// Review: docs/architecture/primitive-types.md, "Boundary adoption audit".
Status TryRasterLayout(RasterInput input, RasterLayout& out) noexcept
{
    using namespace ludus::foundation;
    RasterLayout next;
    if (input.Width == 0 || input.Height == 0)
    {
        out = next;
        return Status::Ok;
    }
    const int64 widePitch = input.Pitch;
    const int64 magnitude = widePitch < 0 ? -widePitch : widePitch;
    usize lastRow{};
    usize sourceExtent{};
    if (TryIntegerCast(magnitude, next.Pitch) == false)
    {
        return Status::ResourceLimit;
    }
    if (next.Pitch < input.Width)
    {
        return Status::BackendFailure;
    }
    if (TryMultiply(static_cast<usize>(input.Width), static_cast<usize>(input.Height), next.CoverageBytes) == false ||
        TryMultiply(static_cast<usize>(input.Height - 1), next.Pitch, lastRow) == false ||
        TryAdd(lastRow, static_cast<usize>(input.Width), sourceExtent) == false ||
        next.CoverageBytes > (~usize{0} >> 1) || sourceExtent > (~usize{0} >> 1))
    {
        return Status::ResourceLimit;
    }
    out = next;
    return Status::Ok;
}
} // namespace ludus::text::internal
