#pragma once

// CPU oracle for edge diffraction in the engine's diffraction test scenes (tests/scenes/diffraction), in float64 at one
// wavelength. It reads a scene source file and gives the wall radiance that geometric optics, the Fresnel-Kirchhoff
// integral and first-order Uniform Theory of Diffraction (UTD) predict, so that a renderer's diffraction can be
// compared with known physics. edge_diffraction_reference.h turns it into reference images and comparisons. The UTD
// coefficient is the one of R. G. Kouyoumjian and P. H. Pathak, "A Uniform Geometrical Theory of Diffraction for an
// Edge in a Perfectly Conducting Surface", Proceedings of the IEEE 62(11), 1448-1461, 1974,
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
// perfect conductor), "hard" the Neumann edge. Unpolarized light averages their intensities. "Absorbing" keeps only the
// coefficient's incident-field terms, the mean of the soft and hard amplitudes: for a half-plane that is the black
// (Kirchhoff) screen's edge, and for other wedges the usual approximation of faces that reflect nothing.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/scene_source/scene_source.h>

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

/// The UTD transition function F(x) = 2 j sqrt(x) exp(j x) G(sqrt(x)) for x >= 0 (Kouyoumjian and Pathak).
/// F(0) = 0 and F(x) tends to 1 as x grows.
[[nodiscard]] DiffractionComplex ComputeUtdTransition(foundation::float64 x) noexcept;

/// The boundary condition of an edge's faces.
enum class EdgeCondition : foundation::uint8
{
    Soft = 0, ///< Dirichlet: the field vanishes on the faces.
    Hard = 1, ///< Neumann: the field's normal derivative vanishes on the faces.
    /// The incident-field terms only: faces that reflect nothing. For a half-plane, the black (Kirchhoff) screen.
    Absorbing = 2,
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

/// The UTD diffraction coefficient D (Kouyoumjian and Pathak), finite on the shadow and reflection
/// boundaries, where the oracle uses the coefficient's limit.
[[nodiscard]] DiffractionComplex ComputeUtdCoefficient(const WedgeDiffraction& diffraction) noexcept;

/// Keller's non-uniform GTD coefficient, which the UTD coefficient approaches away from the boundaries. It is
/// infinite on them.
[[nodiscard]] DiffractionComplex ComputeGtdCoefficient(const WedgeDiffraction& diffraction) noexcept;

/// The exact total field of a unit plane wave from the direction IncidentAngle on a half-plane (n = 2), at
/// DistanceParameterWorld from the edge (Sommerfeld). Its other members' EdgeSine and WedgeN are ignored. Exact for
/// soft and hard edges; for Absorbing it is the corresponding black-screen approximation.
[[nodiscard]] DiffractionComplex ComputeHalfPlaneField(const WedgeDiffraction& diffraction) noexcept;

/// The UTD total field of the same plane wave on a wedge: the incident field where it is lit, the fields reflected by
/// each face where they reach (none for Absorbing), and the diffracted field D exp(-j k rho) / sqrt(rho), with rho =
/// DistanceParameterWorld.
[[nodiscard]] DiffractionComplex ComputeUtdWedgeField(const WedgeDiffraction& diffraction) noexcept;

/// Obstacles per diffraction scene.
inline constexpr foundation::usize MAX_DIFFRACTION_OBSTACLES = 8;
/// Corners of one obstacle's cross-section.
inline constexpr foundation::usize MAX_CROSS_SECTION_CORNERS = 16;

/// A black prism along y: its cross-section in the x-z plane (each corner's Y is 0), strictly convex and
/// counterclockwise with x to the right and z up. Every corner is an edge along y.
struct DiffractionObstacle final
{
    foundation::StaticArray<foundation::math::Vector3d, MAX_CROSS_SECTION_CORNERS> CornersWorld{};
    foundation::usize CornerCount = 0;
};

/// A diffraction test scene read from a scene source file: one one-sided rectangular light facing -z, black prisms
/// along y, and a Lambertian wall facing +z below them, seen by the file's camera. The oracle treats the prisms as
/// infinitely long, so diffraction from their ends (corner diffraction, which UTD does not describe) is left out.
struct EdgeDiffractionScene final
{
    foundation::StaticArray<DiffractionObstacle, MAX_DIFFRACTION_OBSTACLES> Obstacles{};
    foundation::usize ObstacleCount = 0;
    foundation::math::Vector3d LightCenterWorld{}; ///< The light's centre; it emits towards -z.
    foundation::float64 LightHalfWidthWorld = 0;   ///< Half the light's extent along x.
    foundation::float64 LightHalfHeightWorld = 0;  ///< Half the light's extent along y.
    foundation::float64 LightRadiance = 0;         ///< Emitted radiance, W / (sr m^2), equal in R, G and B.
    foundation::float64 WallHeightWorld = 0;       ///< z of the wall.
    foundation::math::Vector3d WallMinimumWorld{}; ///< The wall's lower x and y bounds (Z = WallHeightWorld).
    foundation::math::Vector3d WallMaximumWorld{}; ///< The wall's upper x and y bounds (Z = WallHeightWorld).
    foundation::float64 WallAlbedo = 0;            ///< Lambertian reflectance, equal in R, G and B.
    foundation::math::Vector3d EyeWorld{};
    foundation::math::Vector3d TargetWorld{};
    foundation::math::Vector3d UpWorld{};
    foundation::float64 VerticalFovRadians = 0;
};

/// Why a scene source is not a diffraction scene.
enum class DiffractionSceneStatus : foundation::uint8
{
    Success = 0,
    MissingCamera = 1,       ///< The file has no camera.
    UnsupportedLight = 2,    ///< Not exactly one emitting object: a grey, one-sided, axis-aligned rectangle facing -z.
    UnsupportedWall = 3,     ///< Not exactly one lit object: a grey diffuse axis-aligned rectangle facing +z below.
    UnsupportedObstacle = 4, ///< Another object is not a black, strictly convex prism along y.
    TooManyObstacles = 5,    ///< More than MAX_DIFFRACTION_OBSTACLES, or a cross-section over its corner limit.
};

/// Reads the diffraction scene a parsed scene source describes. outScene is written only on success.
[[nodiscard]] DiffractionSceneStatus BuildEdgeDiffractionScene(const content::scene_source::SceneSource& source,
                                                               EdgeDiffractionScene& outScene) noexcept;

/// What predicts the light that reaches the wall.
enum class DiffractionModel : foundation::uint8
{
    Geometric = 0,      ///< Straight-line visibility: what a ray tracer without diffraction gives.
    Kirchhoff = 1,      ///< Fresnel-Kirchhoff over the single bounded gap between obstacles that share a top plane.
    UtdAbsorbing = 2,   ///< First-order UTD with absorbing (black) faces; the reference for these black scenes.
    UtdSoft = 3,        ///< First-order UTD with soft perfectly conducting faces, including their reflections.
    UtdHard = 4,        ///< The same with hard faces.
    UtdUnpolarized = 5, ///< The mean of the soft and hard intensities: unpolarized light on conducting faces.
};

/// The intensity at a wall point from a point source on the light, relative to the intensity without obstacles.
/// First-order UTD sums geometric optics and one diffraction at each edge, along paths that no obstacle blocks;
/// light diffracted twice (from edge to edge) is left out. Returns false, leaving outTransmission unchanged, when
/// the model does not apply to the scene (Kirchhoff without a single aperture) or the inputs are invalid.
[[nodiscard]] bool TryComputeDiffractionTransmission(const EdgeDiffractionScene& scene,
                                                     DiffractionModel model,
                                                     foundation::float64 wavelengthWorld,
                                                     foundation::math::Vector3d sourceWorld,
                                                     foundation::math::Vector3d wallWorld,
                                                     foundation::float64& outTransmission) noexcept;

/// The most Gauss-Legendre nodes per light piece and axis.
inline constexpr foundation::usize MAX_LIGHT_QUADRATURE_NODES = 32;

/// How a wall radiance is integrated.
struct DiffractionSettings final
{
    DiffractionModel Model = DiffractionModel::UtdAbsorbing;
    foundation::float64 WavelengthWorld = 0.005; ///< Positive, in metres.
    /// Gauss-Legendre nodes per light piece and axis, in [1, MAX_LIGHT_QUADRATURE_NODES]. Each node is a point source:
    /// the light is spatially incoherent (an ordinary thermal emitter), so their intensities add. The light is split
    /// along x wherever an obstacle corner changes a path's visibility, so shadows cost no accuracy.
    foundation::usize LightQuadratureNodes = 6;
};

/// The radiance leaving the wall at a point, in W / (sr m^2): the light's quadrature points weighted by their
/// unobstructed irradiance and the model's transmission, times albedo / pi. Nothing else in the scene reflects light.
/// Returns false, leaving outRadiance unchanged, where TryComputeDiffractionTransmission would, or for a node count
/// out of range or more visibility changes than the oracle tracks (scenes with very many corners).
[[nodiscard]] bool TryComputeDiffractionWallRadiance(const EdgeDiffractionScene& scene,
                                                     const DiffractionSettings& settings,
                                                     foundation::math::Vector3d wallWorld,
                                                     foundation::float64& outRadiance) noexcept;

/// The wall radiance without obstacles, with the same quadrature; zero for a node count out of range.
[[nodiscard]] foundation::float64 ComputeDiffractionFreeWallRadiance(const EdgeDiffractionScene& scene,
                                                                     foundation::math::Vector3d wallWorld,
                                                                     foundation::usize lightQuadratureNodes) noexcept;

/// The wall point that the centre of pixel (column, row) of a width x height image from the scene's camera sees;
/// row 0 is the top. Returns false when the pixel's ray misses the wall or an obstacle hides it.
[[nodiscard]] bool TryFindDiffractionWallPoint(const EdgeDiffractionScene& scene,
                                               foundation::uint32 column,
                                               foundation::uint32 row,
                                               foundation::uint32 width,
                                               foundation::uint32 height,
                                               foundation::math::Vector3d& outWallWorld) noexcept;
} // namespace ludus::graphics::renderer::testing
