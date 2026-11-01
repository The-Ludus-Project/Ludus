#pragma once

// CPU oracle for the steinberg-physical-optics render graph template's scattering core
// (render_graphs/steinberg_physical_optics/main_shaders/wave_optics.slang): beams with Stokes vectors and anisotropic
// Gaussian coherence, the wave BSDF of a Gaussian surface, a sinusoidal grating and a perfectly diffuse surface, the
// shape-matrix transformation on scattering and the maximally incoherent sampling envelope, in float64. Thanks to
// Shlomi Steinberg, Pradeep Sen and Ling-Qi Yan, "Towards Practical Physical-Optics Rendering", ACM Transactions on
// Graphics 41(4), article 132, SIGGRAPH 2022, https://doi.org/10.1145/3528223.3530119 : Definition 3.1, Eqs. (16),
// (20), (22), (25), (28), (29), (31), (34) and Appendix D. The template's source names the departures, which the oracle
// shares. The oracle tests (steinberg_physical_optics_oracle_tests.cpp) check its physics; the GPU parity test
// (steinberg_physical_optics_parity_tests.cpp) runs the template's Slang on the same cases. Path sampling, scene
// queries and spectral accumulation stay in the template's shade.slang and are covered by its GPU image tests.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
/// A complex number for the Fresnel coefficients.
struct OpticsComplex final
{
    foundation::float64 Real = 0;
    foundation::float64 Imaginary = 0;
};

/// Symmetric 2 x 2 matrix (xx, xy, yy).
struct OpticsSymmetric final
{
    foundation::float64 Xx = 0;
    foundation::float64 Xy = 0;
    foundation::float64 Yy = 0;
};

/// Stokes vector (S0, S1, S2, S3).
struct OpticsStokes final
{
    foundation::float64 S0 = 0;
    foundation::float64 S1 = 0;
    foundation::float64 S2 = 0;
    foundation::float64 S3 = 0;
};

/// A beam at one wavelength (wave_optics.slang's Beam).
struct OpticsBeam final
{
    foundation::math::Vector3d PropagationWorld{0, 0, -1}; ///< Unit mean direction of propagation.
    foundation::math::Vector3d TransverseXWorld{1, 0, 0};  ///< Unit local x axis, perpendicular to propagation.
    OpticsStokes Stokes{};
    OpticsSymmetric ShapeX{}; ///< theta_x in m^-2.
    OpticsSymmetric ShapeY{};
    foundation::float64 DistanceWorld = 1; ///< Mean propagation distance r in metres.
};

/// The lobe a parity case scatters through.
enum class OpticsLobe : foundation::uint32
{
    Scattered = 0, ///< Gaussian surface, Eq. (28) with Eq. (22)'s 1 - alpha_direct.
    Diffuse = 1,   ///< Perfectly diffuse, Eq. (32).
    Grating = 2,   ///< Sinusoidal grating, Eq. (34).
    Direct = 3,    ///< Eq. (21)'s specular lobe, weighted by alpha_direct.
};

/// One scattering event of a beam at a surface.
struct OpticsCase final
{
    OpticsBeam Incident{};
    foundation::float64 WavelengthNanometers = 550;
    OpticsLobe Lobe = OpticsLobe::Scattered;
    bool IsWave = true; ///< false evaluates D in its coherent limit, as the radiometric pass does.
    foundation::math::Vector3d NormalWorld{0, 0, 1};
    foundation::math::Vector3d TangentWorld{1, 0, 0}; ///< Unit, perpendicular to the normal.
    foundation::math::Vector3d OutgoingWorld{0, 0, 1};
    OpticsComplex Eta{1.5, 0};
    foundation::float64 RmsHeightWorld = 0;
    foundation::float64 CorrelationLengthWorld = 1e-6;
    foundation::float64 LongitudinalCoherenceWorld = 1e-3;
    foundation::float64 GratingPeriodWorld = 740e-9;
    foundation::float64 GratingHeightWorld = 160e-9;
    foundation::uint32 GratingMaximumOrder = 3;
    foundation::float64 EnvelopeVariance = 1; ///< Variance of the half-vector envelope, in h~ units.
    foundation::float64 Weight = 1;           ///< The lobe's material share.
};

/// What the template computes for one case.
struct OpticsResult final
{
    OpticsBeam Scattered{};
    foundation::float64 DiffractionX = 1;
    foundation::float64 DiffractionY = 1;
    foundation::float64 DiffractionHalf = 1;
    foundation::float64 DirectFraction = 1;
    /// Solid-angle density of drawing the incident direction from the case's envelope (Eq. (31)).
    foundation::float64 EnvelopeDensity = 0;
    /// Characteristic transverse coherence length of the incident beam, in metres.
    foundation::float64 CoherenceLengthWorld = 0;
};

/// Fresnel amplitude reflection coefficients (f_ss, f_pp) at a cosine for a relative index.
void ComputeOpticsFresnel(foundation::float64 cosine,
                          OpticsComplex eta,
                          OpticsComplex& outPerpendicular,
                          OpticsComplex& outParallel) noexcept;
/// Unpolarized reflectance (|f_ss|^2 + |f_pp|^2) / 2.
[[nodiscard]] foundation::float64 ComputeOpticsUnpolarizedReflectance(foundation::float64 cosine,
                                                                      OpticsComplex eta) noexcept;
/// Intensity behind a linear polarizer at angle (radians from the beam's x axis towards y), clamped at zero.
[[nodiscard]] foundation::float64 ApplyOpticsPolarizer(const OpticsStokes& stokes, foundation::float64 angle) noexcept;
/// Bessel function of the first kind J_order(x), x >= 0, as the template evaluates it.
[[nodiscard]] foundation::float64 EvaluateOpticsBessel(foundation::uint32 order, foundation::float64 x) noexcept;
/// Unpolarized natural light with isotropic shape theta (Eq. (25)), built as MakeNaturalBeam builds it.
[[nodiscard]] OpticsBeam MakeOpticsNaturalBeam(foundation::math::Vector3d propagationWorld,
                                               foundation::float64 radiance,
                                               foundation::float64 shape,
                                               foundation::float64 distanceWorld) noexcept;
/// theta = 2 pi Omega / A of a radiator (Eq. (25)).
[[nodiscard]] foundation::float64 ComputeOpticsRadiatorShape(foundation::float64 radiatorAreaWorld,
                                                             foundation::float64 beamSolidAngle) noexcept;
/// theta of sunlight with the given coherence length at 550 nm.
[[nodiscard]] foundation::float64 ComputeOpticsSunShape(foundation::float64 sunCoherenceWorld) noexcept;
/// Mean earth-sun distance the template uses for directional light, in metres.
inline constexpr foundation::float64 OPTICS_SUN_DISTANCE_WORLD = 1.496e11;
/// (r / k) sqrt(min eigenvalue of theta_x, theta_y).
[[nodiscard]] foundation::float64 ComputeOpticsCoherenceLength(const OpticsBeam& beam,
                                                               foundation::float64 wavelengthNanometers) noexcept;
/// Determinant of a symmetric matrix.
[[nodiscard]] foundation::float64 ComputeOpticsDeterminant(const OpticsSymmetric& matrix) noexcept;
/// Scatter the case's incident beam and evaluate every quantity the parity kernel reports.
[[nodiscard]] OpticsResult EvaluateOpticsCase(const OpticsCase& opticsCase) noexcept;
/// The scattered radiance S0 per unit incident radiance of a case, divided by nothing: the BRDF value times the
/// incident S0 (wave_optics.slang's convention). Used by the energy tests.
[[nodiscard]] foundation::float64 EvaluateOpticsScatteredRadiance(const OpticsCase& opticsCase) noexcept;
} // namespace ludus::graphics::renderer::testing
