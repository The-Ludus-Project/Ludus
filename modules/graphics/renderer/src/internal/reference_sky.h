#pragma once

#include "../../shaders/renderer_reference_sky_layout.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting_layout.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
struct ReferenceCaptureDescription;

namespace internal
{
using ReferenceSkyWords = foundation::StaticArray<foundation::math::Vector4, LUDUS_REFERENCE_SKY_WORDS>;

struct ReferenceSky final
{
    ReferenceSkyWords Words{};

    [[nodiscard]] LUDUS_INLINE constexpr bool IsEnabled() const noexcept
    {
        return Words[LUDUS_REFERENCE_SKY_UP_WORLD] != foundation::math::Vector4{};
    }
};

[[nodiscard]] rhi::RasterStatus PrepareReferenceSky(const ReferenceCaptureDescription& description,
                                                    ReferenceSky& outSky) noexcept;
[[nodiscard]] foundation::math::Vector3 EvaluateReferenceSky(const ReferenceSky& sky,
                                                             foundation::math::Vector3 directionWorld) noexcept;
[[nodiscard]] foundation::float64 GetReferenceSkyHorizon(foundation::float64 altitudeMetres) noexcept;
[[nodiscard]] foundation::math::Vector3 EvaluateReferenceSkyTransmittance(foundation::float64 altitudeMetres,
                                                                          foundation::float64 zenithCosine) noexcept;
} // namespace internal
} // namespace ludus::graphics::renderer
