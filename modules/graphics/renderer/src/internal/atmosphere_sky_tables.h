#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>

#include "../../shaders/renderer_gpu_preview_layout.h"

namespace ludus::graphics::renderer::internal
{
/// One row-major table of linear RGB values with the extent of its shader texture.
struct AtmosphereTable final
{
    foundation::Array<foundation::math::Vector3d> Texels;
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;

    /// Size the table; false leaves it unusable.
    [[nodiscard]] bool TryAllocate(foundation::uint32 width, foundation::uint32 height) noexcept;
    /// Texel at (x, y). @pre x < Width and y < Height.
    [[nodiscard]] const foundation::math::Vector3d& Load(foundation::uint32 x, foundation::uint32 y) const noexcept;
};

/// Light along a view segment from the observer: in-scattered radiance per unit solar illuminance and RGB
/// transmittance.
struct AtmosphereViewSample final
{
    foundation::math::Vector3d Radiance{};
    foundation::math::Vector3d Transmittance{1, 1, 1};
};

/// CPU copies of the raster's clear-sky transmittance, multiple-scattering and sky tables
/// (renderer_atmosphere.lusl) for one observer altitude and solar zenith cosine, with their lookups.
/// Values are per unit solar illuminance. Heights are in kilometres above the ground sphere.
class AtmosphereSkyTables final
{
public:
    /// Build every table. hasMultiple false omits the multiple-scattering source from the sky, as the
    /// raster does when the Atmosphere does not light the scene. False leaves the tables unusable.
    /// @pre heightKilometres is within [0, LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES]; solarZenithCosine within [-1, 1].
    [[nodiscard]] bool
    TryBuild(foundation::float64 heightKilometres, foundation::float64 solarZenithCosine, bool hasMultiple) noexcept;
    /// In-scattered radiance toward the observer from directionWorld, without the solar disk (SampleAtmosphereSky).
    /// @pre TryBuild succeeded; every direction is unit length.
    [[nodiscard]] foundation::math::Vector3d SampleSky(foundation::math::Vector3d directionWorld,
                                                       foundation::math::Vector3d upWorld,
                                                       foundation::math::Vector3d sunWorld) const noexcept;
    /// Transmittance to the top of the atmosphere at a height and zenith cosine (SampleTransmittance).
    /// @pre TryBuild succeeded.
    [[nodiscard]] foundation::math::Vector3d SampleTransmittance(foundation::float64 heightKilometres,
                                                                 foundation::float64 zenithCosine) const noexcept;
    /// Cosine of the geometric horizon's zenith angle at a height (ComputeHorizonCosine).
    [[nodiscard]] static foundation::float64 GetHorizonCosine(foundation::float64 heightKilometres) noexcept;
    /// In-scattered radiance per unit solar illuminance and RGB transmittance along one view direction from the
    /// observer, accumulated from the observer to each distance in distancesKilometres, with the same quadrature
    /// step and both scattering sources as the sky. outSamples pairs one sample with each distance.
    /// @pre TryBuild succeeded with hasMultiple; distances are finite, nonnegative and nondecreasing; both views
    ///      have the same size.
    void IntegrateView(foundation::float64 zenithCosine,
                       foundation::float64 solarZenithCosine,
                       foundation::float64 scatteringCosine,
                       foundation::BorrowedView<const foundation::float64> distancesKilometres,
                       foundation::BorrowedView<AtmosphereViewSample> outSamples) const noexcept;

private:
    AtmosphereTable mTransmittance;
    AtmosphereTable mMultiple;
    AtmosphereTable mSky;
    foundation::float64 mHeightKilometres = 0;
};

/// Float4 words of an aerial perspective table (renderer_gpu_preview_layout.h), header included.
inline constexpr foundation::usize AERIAL_PERSPECTIVE_TABLE_WORDS =
    LUDUS_AERIAL_PERSPECTIVE_HEADER_WORDS +
    foundation::usize{LUDUS_AERIAL_PERSPECTIVE_AZIMUTH_COUNT} * LUDUS_AERIAL_PERSPECTIVE_ZENITH_COUNT *
        LUDUS_AERIAL_PERSPECTIVE_SLICE_COUNT * LUDUS_AERIAL_PERSPECTIVE_ENTRY_WORDS;

/// Resolve the haze between an Earth clear-sky observer at eyeWorld and points up to
/// LUDUS_AERIAL_PERSPECTIVE_MAX_METRES away, as the table ApplyAerialPerspective reads.
///
/// Integrates the sky's single and multiple scattering (Hillaire 2020, section 5.3) along each table direction from
/// the observer's altitude, scaled by the Atmosphere's solar illuminance. Every segment starts at the observer's
/// altitude, so segments from other origins use the observer's haze.
/// @param atmosphere Validated as ResolveAtmosphereEnvironment validates it; ShouldLightScene must be true.
/// @param eyeWorld Observer position in world metres.
/// @param outTable AERIAL_PERSPECTIVE_TABLE_WORDS words, written with a present header when Ready.
/// @return The status ResolveAtmosphereEnvironment would return; every status but Ready preserves outTable.
[[nodiscard]] AtmosphereEnvironmentStatus
ResolveAtmosphereAerialPerspective(const AtmosphereDescription& atmosphere,
                                   foundation::math::Vector3 eyeWorld,
                                   foundation::BorrowedView<foundation::math::Vector4> outTable) noexcept;
} // namespace ludus::graphics::renderer::internal
