#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
namespace internal
{
/// Private logical identity access shared with Renderer.
struct Access;
} // namespace internal
/// Prewarmed immutable shader/layout/six-pipeline incarnation, scoped to one renderer.
/// Release detaches public ownership; material/snapshot/GPU uses retain its version.
struct MaterialProgram final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private identity assignment and validation.
    friend struct internal::Access;
};
/// Immutable copied parameters and exact sampled texture/binding incarnation.
/// Copies are borrowed identities; Release/Publish transfers explicit ownership only.
struct MaterialVersion final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private identity assignment and validation.
    friend struct internal::Access;
};
/// Unlit material alpha domain; no screen-door or hidden transparency sorting.
enum class MaterialAlpha : ludus::foundation::uint8
{
    /// Ignore sampled alpha, write/test scene depth and output alpha one.
    Opaque,
    /// Discard below AlphaCutoff, then write/test scene depth and output alpha one.
    Masked,
    /// Straight authored color/texture alpha, premultiplied once; painter Overlay layer.
    Painter,
};
/// Copied unlit instance schema for the installed renderer_material.lusl program.
/// Color textures are linear RGBA8 or sRGB RGBA8; data/coverage formats reject.
struct MaterialDescription final
{
    /// Ready sampled RGBA8 texture; null uses the program's ready white fallback.
    /// Creation retains the exact texture view; caller may destroy its owner afterward.
    rhi::TextureHandle Texture{};
    /// Straight display-linear RGBA in [0,1]; Opaque requires alpha one.
    ludus::foundation::math::Vector4 Tint{1, 1, 1, 1};
    /// UV scale XY and offset ZW, finite; authored UVs use top-left image orientation.
    ludus::foundation::math::Vector4 UvTransform{1, 1, 0, 0};
    /// Independent immutable filter/address/mip state.
    rhi::SamplerDescription Sampler{};
    /// Scene ordering/depth contract.
    MaterialAlpha Alpha = MaterialAlpha::Opaque;
    /// Mask threshold in [0,1], relevant only to Masked; equal alpha survives.
    ludus::foundation::float32 AlphaCutoff = 0.5F;
    /// -1 selects implicit derivative LOD; otherwise a finite initialized level [0,last].
    /// Explicit LOD supports deterministic content previews, not automatic streaming.
    ludus::foundation::float32 Lod = -1;
};
} // namespace ludus::graphics::renderer
