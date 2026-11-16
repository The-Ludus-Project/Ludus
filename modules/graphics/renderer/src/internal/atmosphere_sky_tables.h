#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/vector.hpp>

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

/// CPU copies of the raster's clear-sky transmittance, multiple-scattering and sky tables
/// (renderer_atmosphere.slang) for one observer altitude and solar zenith cosine, with their lookups.
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

private:
    AtmosphereTable mTransmittance;
    AtmosphereTable mMultiple;
    AtmosphereTable mSky;
    foundation::float64 mHeightKilometres = 0;
};
} // namespace ludus::graphics::renderer::internal
