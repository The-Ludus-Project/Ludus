#pragma once

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::testing
{
inline constexpr foundation::uint8 HAIR_MASK_MID_ALPHA = 128;
inline constexpr foundation::uint8 HAIR_MASK_SOLID_ALPHA = 255;
inline constexpr foundation::float32 HAIR_MASK_CUTOFF = 0.5F;
inline constexpr foundation::float32 HAIR_MASK_MISMATCHED_CUTOFF = 0.75F;

struct HairCardCoverageFixture final
{
    static constexpr foundation::usize TEXEL_BYTES = 4;
    foundation::StaticArray<foundation::uint8, TEXEL_BYTES> Texture{{255, 255, 255, HAIR_MASK_MID_ALPHA}};

    void SetMask(SceneShaderMaterial& inoutMaterial) const
    {
        inoutMaterial.Scattering = content::scene_format::MakeHairCardScattering({});
        inoutMaterial.IsAlphaMasked = true;
        inoutMaterial.AlphaCutoff = HAIR_MASK_CUTOFF;
        inoutMaterial.ColorTexture =
            {Texture.GetData(), Texture.GetSize(), Texture.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    }
};
} // namespace ludus::graphics::renderer::testing
