#include <ludus/graphics/text/text_adapter.hpp>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::text
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
Status Convert(ludus::text::Status status) noexcept
{
    using Cpu = ludus::text::Status;
    switch (status)
    {
        case Cpu::Ok:
            return Status::Ready;
        case Cpu::InvalidHandle:
            return Status::InvalidHandle;
        case Cpu::OutOfMemory:
            return Status::OutOfMemory;
        case Cpu::ResourceLimit:
            return Status::CapacityExceeded;
        case Cpu::UnsupportedFont:
        case Cpu::UnsupportedGlyphFormat:
        case Cpu::UnsupportedPresentation:
        case Cpu::UnsupportedText:
            return Status::Unsupported;
        default:
            return Status::InvalidDescription;
    }
}
} // namespace
Status PrepareText(CoverageAtlas& atlas,
                   ludus::text::FontSystem* system,
                   ludus::text::LayoutId layout,
                   uint64 fontVersion,
                   TextQuad* output,
                   usize capacity,
                   usize& count) noexcept
{
    if (fontVersion == 0 || output == nullptr || capacity > CoverageAtlas::MAX_GLYPHS)
    {
        return Status::InvalidDescription;
    }
    ludus::text::LayoutView view;
    auto status = ludus::text::GetLayout(system, layout, &view);
    if (status != ludus::text::Status::Ok)
    {
        return Convert(status);
    }
    if (view.Glyphs.size() > capacity)
    {
        return Status::CapacityExceeded;
    }
    TextQuad scratch[CoverageAtlas::MAX_GLYPHS];
    int64 penX = 0, penY = 0;
    usize used = 0;
    for (const auto& shaped : view.Glyphs)
    {
        AtlasGlyph glyph;
        const GlyphKey key{fontVersion, shaped.GlyphId, view.PixelHeight};
        auto ready = atlas.Find(key, glyph);
        if (ready == Status::NotReady)
        {
            ludus::text::GlyphBitmap bitmap;
            status = ludus::text::RasterizeGlyph(system, view.Font, view.PixelHeight, shaped.GlyphId, &bitmap);
            if (status != ludus::text::Status::Ok)
            {
                return Convert(status);
            }
            ready = atlas.Insert(key,
                                 bitmap.Coverage.data(),
                                 bitmap.Coverage.size(),
                                 bitmap.Width,
                                 bitmap.Height,
                                 bitmap.BearingLeft,
                                 bitmap.BearingTop,
                                 glyph);
        }
        if (ready != Status::Ready)
        {
            return ready;
        }
        const float32 x = static_cast<float32>(static_cast<float64>(penX + shaped.XOffset) /
                                                   ludus::text::ShapedGlyph::UNITS_PER_PIXEL +
                                               glyph.BearingLeft);
        const float32 y = static_cast<float32>(-static_cast<float64>(penY + shaped.YOffset) /
                                                   ludus::text::ShapedGlyph::UNITS_PER_PIXEL -
                                               glyph.BearingTop);
        scratch[used++] = {{x, y, x + static_cast<float32>(glyph.Width), y + static_cast<float32>(glyph.Height)},
                           glyph.Uv,
                           shaped.Cluster};
        penX += shaped.XAdvance;
        penY += shaped.YAdvance;
    }
    for (usize i = 0; i < used; ++i)
    {
        output[i] = scratch[i];
    }
    count = used;
    return Status::Ready;
}
} // namespace ludus::graphics::text
