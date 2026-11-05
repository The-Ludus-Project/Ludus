#pragma once

// CPU oracle for edge diffraction in the single-slit test scene (tests/scenes/diffraction/single-slit.luscene.json),
// in float64 at one wavelength. It gives the wall radiance that geometric optics, the Fresnel-Kirchhoff integral and
// the Uniform Theory of Diffraction (UTD) predict, so that a renderer's diffraction can be compared with known
// physics. The UTD coefficient is the one of R. G. Kouyoumjian and P. H. Pathak, "A Uniform Geometrical Theory of
// Diffraction for an Edge in a Perfectly Conducting Surface", Proceedings of the IEEE 62(11), 1448-1461, 1974,
// https://doi.org/10.1109/PROC.1974.9651 : the wedge coefficient with its transition function, the distance parameter
// of a spherical wave and the diffracted field's spreading factor. Its non-uniform limit is J. B. Keller's,
// "Geometrical Theory of Diffraction", Journal of the Optical Society of America 52(2), 116-130, 1962,
// https://doi.org/10.1364/JOSA.52.000116 . The exact half-plane solution that validates it is A. Sommerfeld's,
// "Mathematische Theorie der Diffraction", Mathematische Annalen 47, 317-374, 1896,
// https://doi.org/10.1007/BF01447273 , in the Fresnel-integral form of M. Born and E. Wolf, Principles of Optics,
// 7th edition, Chapter 11. The Fresnel-Kirchhoff integral is Born and Wolf's Section 8.3. The Fresnel integrals use
// the power series and the complementary-error-function continued fraction of M. Abramowitz and I. A. Stegun,
// Handbook of Mathematical Functions, Sections 7.3 and 7.1.
//
// Conventions: time dependence exp(+j w t), so fields propagate as exp(-j k r). A wedge's angles are measured in the
// plane perpendicular to its edge from its face 0 through the exterior to its face n, which lies at n pi; a
// zero-thickness half-plane has n = 2. "Soft" is the Dirichlet edge (the electric field parallel to the edge for a
// perfect conductor), "hard" the Neumann edge. Unpolarized light averages their intensities.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
/// A complex amplitude.
struct DiffractionComplex final
{
    foundation::float64 Real = 0;
    foundation::float64 Imaginary = 0;
};

/// |value|^2.
[[nodiscard]] foundation::float64 ComputeSquaredMagnitude(DiffractionComplex value) noexcept;

/// The Fresnel integrals at one argument.
struct FresnelIntegrals final
{
    foundation::float64 Cosine = 0; ///< C(x) = integral_0^x cos(pi t^2 / 2) dt.
    foundation::float64 Sine = 0;   ///< S(x) = integral_0^x sin(pi t^2 / 2) dt.
};

/// C(x) and S(x), to about 1e-14 for every finite x.
[[nodiscard]] FresnelIntegrals ComputeFresnelIntegrals(foundation::float64 x) noexcept;

/// G(u) = integral_u^infinity exp(-j t^2) dt for any finite u. G(0) = (sqrt(pi) / 2) exp(-j pi / 4).
[[nodiscard]] DiffractionComplex ComputeFresnelTail(foundation::float64 u) noexcept;

/// The UTD transition function F(x) = 2 j sqrt(x) exp(j x) G(sqrt(x)) for x >= 0 (Kouyoumjian and Pathak, Eq. (27)).
/// F(0) = 0 and F(x) tends to 1 as x grows.
[[nodiscard]] DiffractionComplex ComputeUtdTransition(foundation::float64 x) noexcept;

/// The boundary condition of an edge's faces.
enum class EdgeCondition : foundation::uint8
{
    Soft = 0, ///< Dirichlet: the field vanishes on the faces.
    Hard = 1, ///< Neumann: the field's normal derivative vanishes on the faces.
};

/// One edge diffraction, described in the edge's frame.
struct WedgeDiffraction final
{
    foundation::float64 ObservationAngle = 0; ///< phi in [0, n pi], radians from face 0.
    foundation::float64 IncidentAngle = 0;    ///< phi' in [0, n pi], radians from face 0.
    /// The distance parameter L in metres: s for a plane wave, s s' sin^2(beta0) / (s + s') for a spherical one.
    foundation::float64 DistanceParameterWorld = 1;
    /// sin(beta0) of the angle between the incident ray and the edge; 1 at normal incidence.
    foundation::float64 EdgeSine = 1;
    foundation::float64 WedgeN = 2;     ///< The exterior angle in units of pi; in (1, 2] for a physical wedge.
    foundation::float64 Wavenumber = 1; ///< k = 2 pi / wavelength, in radians per metre.
    EdgeCondition Condition = EdgeCondition::Soft;
};

/// The UTD diffraction coefficient D (Kouyoumjian and Pathak, Eq. (25)), finite on the shadow and reflection
/// boundaries, where the oracle uses the coefficient's limit.
[[nodiscard]] DiffractionComplex ComputeUtdCoefficient(const WedgeDiffraction& diffraction) noexcept;

/// Keller's non-uniform GTD coefficient, which the UTD coefficient approaches away from the boundaries. It is
/// infinite on them.
[[nodiscard]] DiffractionComplex ComputeGtdCoefficient(const WedgeDiffraction& diffraction) noexcept;

/// The exact total field of a unit plane wave from the direction IncidentAngle on a half-plane (n = 2), at
/// DistanceParameterWorld from the edge (Sommerfeld). Its other members' EdgeSine and WedgeN are ignored.
[[nodiscard]] DiffractionComplex ComputeHalfPlaneField(const WedgeDiffraction& diffraction) noexcept;

/// The UTD total field of the same plane wave on a wedge: the incident field where it is lit, the fields reflected by
/// each face where they reach, and the diffracted field D exp(-j k rho) / sqrt(rho), with rho = DistanceParameterWorld.
[[nodiscard]] DiffractionComplex ComputeUtdWedgeField(const WedgeDiffraction& diffraction) noexcept;

/// The single-slit test scene, as tests/scenes/diffraction/single-slit.luscene.json authors it. The jaws' edges lie
/// along y at x = +-SlitHalfWidthWorld, z = 0; each jaw's face 0 is its face at z = 0 towards the light and its face n
/// the bevel towards the wall. The oracle treats the edges as infinitely long: the jaws span 3 m, and diffraction
/// from their ends (corner diffraction, which UTD does not describe) is left out.
struct SingleSlitScene final
{
    foundation::float64 WavelengthWorld = 0.005;    ///< The design wavelength, metres.
    foundation::float64 LightHeightWorld = 0.5;     ///< z of the light.
    foundation::float64 LightHalfSizeWorld = 0.015; ///< The square light spans +- this in x and y.
    foundation::float64 LightRadiance = 10000;      ///< One-sided emitted radiance towards -z, W / (sr m^2).
    foundation::float64 SlitHalfWidthWorld = 0.025; ///< The jaws' edges lie at x = +- this.
    foundation::float64 WedgeN = 11.0 / 6.0;        ///< 30 degree jaw edges: exterior angle 330 degrees.
    foundation::float64 WallHeightWorld = -1;       ///< z of the wall.
    foundation::float64 WallAlbedo = 0.8;           ///< Lambertian reflectance of the wall.
};

/// What predicts the light that reaches the wall.
enum class SlitModel : foundation::uint8
{
    Geometric = 0,      ///< Straight-line visibility through the slit: what a ray tracer without diffraction gives.
    Kirchhoff = 1,      ///< Fresnel-Kirchhoff over the slit opening (a black, infinitely thin screen).
    UtdSoft = 2,        ///< Geometric optics plus UTD diffraction from both edges, soft edges.
    UtdHard = 3,        ///< The same with hard edges.
    UtdUnpolarized = 4, ///< The mean of the soft and hard intensities: unpolarized light.
};

/// The intensity at a wall point from a point source on the light, relative to the intensity without the jaws.
/// @pre sourceWorld lies above the jaws (z > 0) and wallWorld on the wall.
[[nodiscard]] foundation::float64 ComputeSlitTransmission(const SingleSlitScene& scene,
                                                          SlitModel model,
                                                          foundation::math::Vector3d sourceWorld,
                                                          foundation::math::Vector3d wallWorld) noexcept;

/// The radiance leaving the wall at a point, in W / (sr m^2): the light integrated with a midpoint rule of
/// subdivisions^2 cells, each a spatially incoherent point source (an ordinary thermal emitter), times the
/// transmission of the model. The wall's own interreflection is absent: everything else in the scene is black.
[[nodiscard]] foundation::float64 ComputeSlitWallRadiance(const SingleSlitScene& scene,
                                                          SlitModel model,
                                                          foundation::math::Vector3d wallWorld,
                                                          foundation::usize subdivisions) noexcept;

/// The wall radiance without the jaws, with the same quadrature.
[[nodiscard]] foundation::float64 ComputeSlitFreeWallRadiance(const SingleSlitScene& scene,
                                                              foundation::math::Vector3d wallWorld,
                                                              foundation::usize subdivisions) noexcept;
} // namespace ludus::graphics::renderer::testing
