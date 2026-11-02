#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
/// Scalar Gaussian beam in its transverse principal frame. All lengths use metres.
/// This profile implements paraxial propagation; it does not model polarization.
struct WaveBeam final
{
    foundation::float64 WavelengthWorld = 0;         ///< Positive wavelength in metres.
    foundation::float64 SigmaXWorldSquared = 0;      ///< Initial shape eigenvalue, not intensity variance (Sigma / 2).
    foundation::float64 SigmaYWorldSquared = 0;      ///< Second initial shape eigenvalue.
    foundation::float64 CorrelationWorldSquared = 0; ///< rho; increases by distance / wavenumber.
};

/// Principal intensity variances and position/wavevector covariances of a propagated beam.
struct WaveMoments final
{
    foundation::float64 VarianceXWorldSquared = 0; ///< Principal spatial intensity variance along X, in m^2.
    foundation::float64 VarianceYWorldSquared = 0; ///< Principal spatial intensity variance along Y, in m^2.
    foundation::float64 CovarianceX = 0;           ///< Dimensionless position/wavevector covariance.
    foundation::float64 CovarianceY = 0;           ///< Dimensionless position/wavevector covariance along Y.
};

/// Complex scalar field; squared magnitude is irradiance relative to the incident peak.
struct WaveField final
{
    foundation::float64 Real = 0;      ///< Real field amplitude relative to the incident peak amplitude.
    foundation::float64 Imaginary = 0; ///< Imaginary field amplitude in the same normalization.
};

/// Finite, coherently illuminated aperture in its own orthonormal transverse frame.
/// Midpoint quadrature covers +/- HalfWidthWorld and +/- HalfHeightWorld.
struct WaveAperture final
{
    foundation::float64 WavelengthWorld = 0;    ///< Positive wavelength in metres.
    foundation::float64 SigmaXWorldSquared = 0; ///< Positive waist shape eigenvalue along X, in m^2.
    foundation::float64 SigmaYWorldSquared = 0; ///< Positive waist shape eigenvalue along Y, in m^2.
    foundation::float64 HalfWidthWorld = 0;     ///< Positive transverse integration half width, in metres.
    foundation::float64 HalfHeightWorld = 0;    ///< Positive transverse integration half height, in metres.
    foundation::uint32 Resolution = 0;          ///< Cells per axis; square grid, row-major x fastest.
};

/// Propagate the beam by a finite nonnegative distance using Steinberg and Pharr's Eqs. (19)-(21).
/// Allocation-free and thread-independent. Invalid/overflowing input preserves both outputs.
[[nodiscard]] bool TryPropagateWaveBeam(const WaveBeam& beam,
                                        foundation::float64 distanceWorld,
                                        WaveBeam& outBeam,
                                        WaveMoments& outMoments) noexcept;

/// Evaluate a scalar Fresnel aperture integral with coherent summation before squaring.
/// receiverApertureWorld is (transverse x, transverse y, positive axial distance), in metres.
/// transmission contains Resolution^2 real amplitude transmissions in [0,1], borrowed for this call.
/// Uses midpoint quadrature of a Gaussian waist field, with the common axial phase omitted.
/// Returns false for invalid/nonfinite inputs, output overflow or phase undersampling; outputs stay unchanged.
/// Refine Resolution or increase wavelength/distance to repair undersampling. No allocation or shared state.
/// This is a single thin-screen scalar profile, not the paper's full multi-region polarimetric solver.
[[nodiscard]] bool TryEvaluateWaveAperture(const WaveAperture& aperture,
                                           foundation::BorrowedView<const foundation::float64> transmission,
                                           foundation::math::Vector3d receiverApertureWorld,
                                           WaveField& outField) noexcept;
} // namespace ludus::graphics::renderer
