#pragma once

// Light authoring data and the selected light list, split from lighting.hpp so
// code that only stores lights does not parse the lighting and raster APIs.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting_layout.h>

namespace ludus::graphics::renderer
{
/// Most lights one LightSelection holds and the light admission functions accept.
inline constexpr ludus::foundation::uint32 MAX_DIRECT_LIGHTS = CLUSTERED_LIGHT_COUNT;
/// Default point/spot range in meters for a new DirectLight.
inline constexpr ludus::foundation::float32 DEFAULT_LIGHT_RANGE_METERS = 10;
/// Default spot inner half-angle cosine for a new DirectLight.
inline constexpr ludus::foundation::float32 DEFAULT_SPOT_INNER_COSINE = 0.9F;
/// Default spot outer half-angle cosine for a new DirectLight.
inline constexpr ludus::foundation::float32 DEFAULT_SPOT_OUTER_COSINE = 0.8F;

/// Punctual RGB light model; distances and positions use meters.
enum class LightKind : ludus::foundation::uint8
{
    /// Uniform incident illuminance in lux; Direction points from light toward scene.
    Directional,
    /// Isotropic luminous intensity in candela, with bounded smooth range falloff.
    Point,
    /// On-axis candela with a squared smooth cone; angles expressed as cosines.
    Spot,
    /// Deterministic area quadrature: Intensity is RGB nits times weighted square metres;
    /// outgoing Direction supplies the Lambertian emitter cosine. Not a delta authoring source.
    AreaOneSided,
    /// Same weighted quadrature with the absolute emitter cosine on both sides.
    AreaTwoSided,
};
/// Copied finite light authoring data, independent of scene ownership.
struct DirectLight final
{
    /// Unique nonzero source identity within the submitted list; defines stable selection.
    ludus::foundation::uint64 SourceId = 0;
    /// Direct evaluation model.
    LightKind Kind = LightKind::Directional;
    /// Display-linear RGB hue, each channel in [0,1].
    ludus::foundation::math::Vector3 Color{1, 1, 1};
    /// Lux for directional, candela for point/spot, nits*m² for area samples; [0,1000000].
    ludus::foundation::float32 Intensity = 1;
    /// Position in meters, each component in [-1000000,1000000].
    ludus::foundation::math::Vector3 Position{};
    /// Nonzero finite direction for directional/spot/area; normalized during admission.
    ludus::foundation::math::Vector3 Direction{0, 0, -1};
    /// Point/spot range in meters, [0.01,1000000]; attenuation becomes zero at range.
    ludus::foundation::float32 Range = DEFAULT_LIGHT_RANGE_METERS;
    /// Spot inner half-angle cosine; -1 < OuterCos < InnerCos <= 1.
    ludus::foundation::float32 InnerCos = DEFAULT_SPOT_INNER_COSINE;
    /// Spot outer half-angle cosine, distinct from InnerCos.
    ludus::foundation::float32 OuterCos = DEFAULT_SPOT_OUTER_COSINE;
};
/// Stable bounded forward list; entries sorted by SourceId, not brightness or opaque depth.
struct LightSelection final
{
    /// L4 selects at most eight; L5 clustered materials report all admitted lights (up to 128).
    DirectLight Lights[MAX_DIRECT_LIGHTS]{};
    /// Number of initialized entries in Lights.
    ludus::foundation::uint32 Count = 0;
    /// Submitted count minus Count; positive means a declared quality reduction.
    ludus::foundation::uint32 Dropped = 0;
    /// L5 cells using full-list fallback; zero for L4 selection.
    ludus::foundation::uint32 OverflowCells = 0;
    /// L5 written cell/light entries; zero for L4 selection.
    ludus::foundation::uint32 Assignments = 0;
};

/// Most panel luminaires one scene submits to scene-shader passes.
inline constexpr ludus::foundation::uint32 MAX_PANEL_LUMINAIRES = 8;
/// Largest admitted absolute panel center coordinate in metres, the analytic lights' position bound.
inline constexpr ludus::foundation::float32 MAX_PANEL_LUMINAIRE_COORDINATE = 1000000;
/// Largest admitted panel half extent in metres.
inline constexpr ludus::foundation::float32 MAX_PANEL_LUMINAIRE_HALF_EXTENT = 1000;
/// Smallest admitted panel half extent in metres; the panel frame divides by it.
inline constexpr ludus::foundation::float32 MIN_PANEL_LUMINAIRE_HALF_EXTENT = 0.001F;
/// Largest admitted radiance scale, unitless.
inline constexpr ludus::foundation::float32 MAX_PANEL_LUMINAIRE_RADIANCE_SCALE = 1000000;
/// Smallest admitted beam width in slope units; the beams' Gaussian Cholesky factors divide by it.
inline constexpr ludus::foundation::float32 MIN_PANEL_LUMINAIRE_BEAM_WIDTH = 0.001F;
/// Largest admitted beam width in slope units.
inline constexpr ludus::foundation::float32 MAX_PANEL_LUMINAIRE_BEAM_WIDTH = 1;
/// Largest admitted beam steering, spread and twist, in slope units (per half extent for spread and twist).
inline constexpr ludus::foundation::float32 MAX_PANEL_LUMINAIRE_BEAM_SLOPE = 2;
/// Default panel half extent in metres: a 0.7 m panel, as in the slice-sampling-luminaire template's test room.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_HALF_EXTENT = 0.35F;
/// Default radiance scale, unitless.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_RADIANCE_SCALE = 6;
/// Default beam width in slope units; with the template's tile factors the widths span 0.016 to 0.024, as in
/// Steinberg's section 6.4.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_BEAM_WIDTH = 0.02F;
/// Default beam steering in slope units.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_BEAM_STEERING = 0.12F;
/// Default beam spread in slope units per half extent.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_BEAM_SPREAD = 0.08F;
/// Default beam twist in slope units per half extent.
inline constexpr ludus::foundation::float32 DEFAULT_PANEL_LUMINAIRE_BEAM_TWIST = 0.04F;

/// Analytic directional luminaire: a square exit surface whose tiles emit position-dependent affine Gaussian beams,
/// after Shlomi Steinberg, "Slice Sampling: Importance sampling with an auxiliary map" (draft, University of
/// Waterloo), section 6.2. It is not scene geometry: camera rays do not see it and it does not occlude. Only scene
/// shaders that read it, such as the slice-sampling-luminaire render-graph template, are lit by it; the raster,
/// clustered and reference renderers ignore it. The template owns the per-tile tables; these values place the panel
/// and scale its beams. See
/// docs/development/render-graph-authoring.md#sample-a-directional-luminaire-with-slice-sampling.
struct PanelLuminaire final
{
    /// Unique nonzero source identity within the submitted list; defines the stable order.
    ludus::foundation::uint64 SourceId = 0;
    /// Panel center in metres, each component in [-MAX_PANEL_LUMINAIRE_COORDINATE,MAX_PANEL_LUMINAIRE_COORDINATE].
    ludus::foundation::math::Vector3 CenterWorld{};
    /// Nonzero finite panel tangent (the tiles' columns); normalized during admission.
    ludus::foundation::math::Vector3 TangentWorld{1, 0, 0};
    /// Panel bitangent (the tiles' rows), not parallel to TangentWorld; admission makes it orthonormal to the tangent.
    /// The panel emits along cross(TangentWorld, BitangentWorld).
    ludus::foundation::math::Vector3 BitangentWorld{0, -1, 0};
    /// Half the panel side in metres, in [MIN_PANEL_LUMINAIRE_HALF_EXTENT,MAX_PANEL_LUMINAIRE_HALF_EXTENT].
    ludus::foundation::float32 HalfExtentWorld = DEFAULT_PANEL_LUMINAIRE_HALF_EXTENT;
    /// Multiplies every tile's radiance, in [0,MAX_PANEL_LUMINAIRE_RADIANCE_SCALE]; it does not change sampling.
    ludus::foundation::float32 RadianceScale = DEFAULT_PANEL_LUMINAIRE_RADIANCE_SCALE;
    /// Gaussian width scale of every tile's beams in slope units, in
    /// [MIN_PANEL_LUMINAIRE_BEAM_WIDTH,MAX_PANEL_LUMINAIRE_BEAM_WIDTH].
    ludus::foundation::float32 BeamWidth = DEFAULT_PANEL_LUMINAIRE_BEAM_WIDTH;
    /// Slope offset of each tile's beams along its steering direction, in [0,MAX_PANEL_LUMINAIRE_BEAM_SLOPE].
    ludus::foundation::float32 BeamSteering = DEFAULT_PANEL_LUMINAIRE_BEAM_STEERING;
    /// Slope change per half extent of panel position, in [0,MAX_PANEL_LUMINAIRE_BEAM_SLOPE]. Nonnegative spread and
    /// twist keep the beams from focusing into a caustic in front of the panel.
    ludus::foundation::float32 BeamSpread = DEFAULT_PANEL_LUMINAIRE_BEAM_SPREAD;
    /// Rotational slope change per half extent of panel position, in [0,MAX_PANEL_LUMINAIRE_BEAM_SLOPE].
    ludus::foundation::float32 BeamTwist = DEFAULT_PANEL_LUMINAIRE_BEAM_TWIST;
};
/// Admitted panel luminaires in SourceId order, with orthonormal frames.
struct PanelLuminaireSelection final
{
    /// The first Count entries are initialized.
    PanelLuminaire Luminaires[MAX_PANEL_LUMINAIRES]{};
    /// Number of initialized entries in Luminaires.
    ludus::foundation::uint32 Count = 0;
};
} // namespace ludus::graphics::renderer
