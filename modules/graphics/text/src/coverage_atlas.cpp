#include <ludus/graphics/text/coverage_atlas.hpp>

#include <ludus/foundation/math/scalar.hpp>

#include <new>

namespace ludus::graphics::text
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
struct CoverageAtlas::State final
{
    struct Cell final
    {
        GlyphKey Key;
        AtlasGlyph Glyph;
    };
    uint8 Bytes[EXTENT * EXTENT]{};
    Cell Cells[MAX_GLYPHS];
    usize Count = 0, Occupied = 0;
    uint32 X = 0, Y = 0, Shelf = 0;
};
CoverageAtlas::CoverageAtlas() noexcept = default;
CoverageAtlas::~CoverageAtlas() = default;
Status CoverageAtlas::Initialize() noexcept
{
    if (mState.Get() == nullptr)
    {
        mState.Reset(new (std::nothrow) State);
    }
    return mState ? Status::Ready : Status::OutOfMemory;
}
void CoverageAtlas::Reset() noexcept
{
    if (mState)
    {
        for (auto& byte : mState->Bytes)
        {
            byte = 0;
        }
        mState->Count = mState->Occupied = 0;
        mState->X = mState->Y = mState->Shelf = 0;
    }
}
Status CoverageAtlas::Find(GlyphKey key, AtlasGlyph& output) const noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (key.FontVersion == 0 || key.PixelHeight == 0 || key.PixelHeight > 256)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < mState->Count; ++i)
    {
        const auto& cell = mState->Cells[i];
        if (cell.Key.FontVersion == key.FontVersion && cell.Key.Glyph == key.Glyph &&
            cell.Key.PixelHeight == key.PixelHeight)
        {
            output = cell.Glyph;
            return Status::Ready;
        }
    }
    return Status::NotReady;
}
Status CoverageAtlas::Insert(GlyphKey key,
                             const uint8* bytes,
                             usize byteCount,
                             uint32 width,
                             uint32 height,
                             int32 bearingLeft,
                             int32 bearingTop,
                             AtlasGlyph& output) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (key.FontVersion == 0 || key.PixelHeight == 0 || key.PixelHeight > 256 || width > EXTENT - 2 ||
        height > EXTENT - 2 || byteCount != static_cast<usize>(width) * height || (byteCount != 0 && bytes == nullptr))
    {
        return Status::InvalidDescription;
    }
    const auto found = Find(key, output);
    if (found == Status::Ready)
    {
        return found;
    }
    auto& s = *mState;
    if (s.Count == MAX_GLYPHS)
    {
        return Status::CapacityExceeded;
    }
    auto x = s.X, y = s.Y, shelf = s.Shelf;
    AtlasGlyph glyph{{}, width, height, bearingLeft, bearingTop};
    if (byteCount != 0)
    {
        // Thanks to Manny Ko, "A Fast and High-Quality Texture Atlasing
        // Algorithm", Game Engine Gems 3 (2016), chapter 9, pp. 111–120,
        // sections 9.4–9.5: reserve filtering footprints before packing. We use shelves
        // for append-only rectangular glyphs, not the chapter's offline packer.
        // Context and bibliography: docs/architecture/renderer-systems-gems-review.md.
        const auto w = width + 2, h = height + 2;
        if (x + w > EXTENT)
        {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + h > EXTENT)
        {
            return Status::CapacityExceeded;
        }
        glyph.Uv = {static_cast<float32>(x + 1) / EXTENT,
                    static_cast<float32>(y + 1) / EXTENT,
                    static_cast<float32>(x + 1 + width) / EXTENT,
                    static_cast<float32>(y + 1 + height) / EXTENT};
        for (usize row = 0; row < height; ++row)
        {
            for (usize column = 0; column < width; ++column)
            {
                s.Bytes[(y + 1 + row) * EXTENT + x + 1 + column] = bytes[row * width + column];
            }
        }
        s.X = x + w;
        s.Y = y;
        s.Shelf = math::Max(shelf, h);
        s.Occupied += static_cast<usize>(w) * h;
    }
    s.Cells[s.Count++] = {key, glyph};
    output = glyph;
    return Status::Ready;
}
Status CoverageAtlas::Publish(rhi::DeviceHandle device, rhi::TextureHandle& output) const noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    return rhi::CreateTexture(device,
                              {EXTENT, EXTENT, rhi::RasterFormat::R8Unorm},
                              {{mState->Bytes, sizeof(mState->Bytes)}, EXTENT},
                              output);
}
usize CoverageAtlas::GlyphCount() const noexcept
{
    return mState ? mState->Count : 0;
}
usize CoverageAtlas::OccupiedTexels() const noexcept
{
    return mState ? mState->Occupied : 0;
}
const uint8* CoverageAtlas::Coverage() const noexcept
{
    return mState ? mState->Bytes : nullptr;
}
Status AppendText(renderer::OverlayList& list,
                  const TextQuad* quads,
                  usize count,
                  math::Vector2 baseline,
                  math::Vector4 color,
                  uint32 width,
                  uint32 height,
                  const renderer::OverlayStyle& style) noexcept
{
    if ((count != 0 && quads == nullptr) || count > CoverageAtlas::MAX_GLYPHS || math::IsFinite(baseline) == false ||
        math::IsFinite(color) == false || color.X < 0 || color.X > 1 || color.Y < 0 || color.Y > 1 || color.Z < 0 ||
        color.Z > 1 || color.W < 0 || color.W > 1 || width == 0 || height == 0 ||
        static_cast<uint8>(style.Depth) > static_cast<uint8>(renderer::OverlayDepth::ReverseZ))
    {
        return Status::InvalidDescription;
    }
    if (list.Vertices() == nullptr)
    {
        return Status::NotReady;
    }
    // A bounded preflight keeps an invalid later glyph from partially appending.
    usize visible = 0;
    for (usize i = 0; i < count; ++i)
    {
        const auto& q = quads[i];
        const auto& r = q.Rectangle;
        const auto& uv = q.Uv;
        const math::Vector4 shifted{r.X + baseline.X, r.Y + baseline.Y, r.Z + baseline.X, r.W + baseline.Y};
        if (math::IsFinite(r) == false || math::IsFinite(shifted) == false || math::IsFinite(uv) == false ||
            r.Z < r.X || r.W < r.Y || uv.X < 0 || uv.Y < 0 || uv.Z > 1 || uv.W > 1 || uv.Z < uv.X || uv.W < uv.Y)
        {
            return Status::InvalidDescription;
        }
        const math::Vector4 clip{static_cast<float32>(2.0 * static_cast<float64>(shifted.X) / width - 1),
                                 static_cast<float32>(1 - 2.0 * static_cast<float64>(shifted.Y) / height),
                                 static_cast<float32>(2.0 * static_cast<float64>(shifted.Z) / width - 1),
                                 static_cast<float32>(1 - 2.0 * static_cast<float64>(shifted.W) / height)};
        if (math::IsFinite(clip) == false)
        {
            return Status::InvalidDescription;
        }
        if (r.Z != r.X && r.W != r.Y)
        {
            ++visible;
        }
    }
    if (visible * 6 > renderer::OverlayList::MAX_VERTICES - list.VertexCount() ||
        (visible != 0 && list.CommandCount() == renderer::OverlayList::MAX_COMMANDS))
    {
        return Status::CapacityExceeded;
    }
    for (usize i = 0; i < count; ++i)
    {
        const auto& q = quads[i];
        const auto& r = q.Rectangle;
        const auto status = list.Quad({r.X + baseline.X, r.Y + baseline.Y, r.Z + baseline.X, r.W + baseline.Y},
                                      q.Uv,
                                      color,
                                      width,
                                      height,
                                      true,
                                      style);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    return Status::Ready;
}
} // namespace ludus::graphics::text
