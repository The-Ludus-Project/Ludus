#pragma once

// The display image a RasterViewport hands out, split from viewport.hpp so code
// that only holds or forwards a frame does not parse the whole Renderer.

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
class RasterViewport;

/// One immutable display image lease. Copies borrow; release the original once.
struct ViewportFrame final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    friend class RasterViewport; ///< Validates the private lease identity.
};
/// Borrowed GPU display image, valid until its ViewportFrame is released.
struct ViewportImage final
{
    rhi::TextureHandle Texture{};         ///< Read-only sRGB-encoded, tone-mapped RGBA8Unorm image; never destroy it.
    rhi::SubmissionToken Producer{};      ///< Completed producer work; consumer completion is separate.
    ludus::foundation::uint32 Width = 0;  ///< Physical width in pixels, top-left origin.
    ludus::foundation::uint32 Height = 0; ///< Physical height in pixels.
};
} // namespace ludus::graphics::renderer
