#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/lighting_layout.h>

namespace ludus::graphics::renderer
{
/// An unresolved planet on the distant celestial boundary.
struct PlanetBackgroundDescription final
{
    /// Nonzero finite world direction toward a visible source. Normalized during preparation.
    foundation::math::Vector3 DirectionWorld{};
    /// Finite nonnegative linear RGB illuminance in lux before extinction. Zero omits this point.
    foundation::math::Vector3 Illuminance{};
};

/// Stars, planets, the Milky Way and an optional Moon supply scene-linear radiance beyond the atmosphere shell.
struct CelestialBackgroundDescription final
{
    /// Enable the celestial boundary and perspective views from orbit.
    bool IsEnabled = false;
    /// Finite nonnegative procedural star peak luminance in scene-linear nits. Zero disables stars.
    foundation::float32 StarRadiance = 0;
    /// Nonzero finite world direction toward the Moon, normalized when its disk is present.
    foundation::math::Vector3 DirectionToMoonWorld{0, 0, 1};
    /// Finite nonnegative linear RGB luminance in nits at unit lunar surface lighting cosine.
    foundation::math::Vector3 LunarPeakRadiance{};
    /// Apparent lunar radius in radians. Zero omits the disk. A positive radius must be less than half pi.
    foundation::float32 LunarAngularRadiusRadians = 0;
    /// Procedural Milky Way brightness scale in scene-linear nits. Finite and nonnegative. Zero omits the band.
    foundation::float32 MilkyWayRadiance = 0;
    /// Finite nonzero world direction toward the galactic north pole, used when the Milky Way is enabled.
    foundation::math::Vector3 GalacticNorthWorld{0, 1, 0};
    /// Finite nonzero world direction toward the galactic center. Its projection onto the galactic plane must
    /// be nonzero when the Milky Way is enabled. The procedural stars then also use this celestial frame.
    foundation::math::Vector3 GalacticCenterWorld{0, 0, -1};
    /// Optional background points. They receive the same view transmittance and occlusion as the stars.
    foundation::StaticArray<PlanetBackgroundDescription, ATMOSPHERE_PLANET_CAPACITY> Planets{};
    /// Nominal apparent lunar radius of one-quarter degree, from NASA Science's Hubble Glossary.
    static constexpr foundation::float32 DEFAULT_LUNAR_RADIUS_RADIANS = 0.004363323F;
    /// Maximum perspective observer altitude in metres for the orbital background.
    static constexpr foundation::float64 MAXIMUM_ORBITAL_ALTITUDE_METRES = 1'000'000'000;
    /// Optional direction from the Moon toward the Sun. Zero uses AtmosphereDescription::DirectionToSunWorld.
    foundation::math::Vector3 DirectionToSunAtMoonWorld{};
};

/// One homogeneous region of a CloudLayerTestField.
struct CloudLayerTestRegion final
{
    /// Extinction at unit layer density in inverse metres, finite and nonnegative. It replaces
    /// CloudLayerDescription::ExtinctionPerMetre inside the region; Density and the edge fade still multiply it.
    foundation::float32 ExtinctionPerMetre = 0;
    /// Finite nonnegative scene-linear RGB source radiance in nits. The region emits ExtinctionPerMetre times this
    /// per metre, so a homogeneous region of transmittance T alone contributes SourceRadiance * (1 - T). The source
    /// adds to the Sun's single scattering and is independent of the Atmosphere's illuminance.
    foundation::math::Vector3 SourceRadiance{};
};

/// @brief Renderer validation fixture: two homogeneous regions of a cloud layer on either side of one plane.
///
/// World extraction and the Editor never author it; the `[cloud-layer]` tests use it to check transmittance through
/// a nonuniform layer and the front-to-back order of two emitting, attenuating regions. A layer point p in the
/// CloudLayerDescription frame (metres) lies in Regions[0] when dot(p, SplitNormalLocal) < SplitOffsetMetres and in
/// Regions[1] otherwise. The view march splits its steps at the plane, so each region is integrated as homogeneous
/// and its transmittance and source are exact. The Sun path toward a step is split at the plane too.
struct CloudLayerTestField final
{
    /// Number of regions, one on each side of the plane.
    static constexpr foundation::usize REGION_COUNT = 2;
    /// False keeps the layer homogeneous. Other fields are ignored when false.
    bool IsEnabled = false;
    /// Finite nonzero plane normal in the layer frame (+X, +Y up, +Z), normalized when packed.
    foundation::math::Vector3 SplitNormalLocal{0, 0, 1};
    /// Finite signed distance in metres of the plane from the layer origin along the normalized normal.
    foundation::float32 SplitOffsetMetres = 0;
    /// Regions[0] lies on the negative side of the plane, Regions[1] on the positive side.
    foundation::StaticArray<CloudLayerTestRegion, REGION_COUNT> Regions{};
};

/// @brief One bounded, horizontal slab of homogeneous cloud in the Atmosphere's surface frame.
///
/// The slab is flat in the plane tangent to the ground at AtmosphereDescription::SurfaceAnchorWorld: its frame has
/// the anchor as origin, SurfaceUpWorld as +Y and AxisXWorld (made perpendicular to up) as +X; +Z is X cross Y. It
/// occupies |x| <= SizeXMetres / 2, |z| <= SizeZMetres / 2 and BaseHeightMetres <= y <= BaseHeightMetres +
/// ThicknessMetres. Within EdgeFadeMetres of a side the density falls smoothly to zero; the base and top are sharp.
/// The medium is grey: extinction Density * ExtinctionPerMetre in inverse metres, a fraction SingleScatteringAlbedo
/// of it scattering through a normalized Henyey-Greenstein phase function in inverse steradians. The renderer lights
/// it by single scattering of the Atmosphere's Sun, attenuated by the air and by the slab towards the Sun, and
/// composes it with the clear sky along each view ray. See docs/architecture/world-environment.md for the transport
/// contract and its current limits.
struct CloudLayerDescription final
{
    /// Default base height in metres, a low stratocumulus deck.
    static constexpr foundation::float32 DEFAULT_BASE_HEIGHT_METRES = 1500;
    /// Default thickness in metres.
    static constexpr foundation::float32 DEFAULT_THICKNESS_METRES = 1000;
    /// Default full size along X and Z in metres.
    static constexpr foundation::float32 DEFAULT_SIZE_METRES = 40000;
    /// Default edge fade in metres.
    static constexpr foundation::float32 DEFAULT_EDGE_FADE_METRES = 2000;
    /// Default extinction at unit density in inverse metres: an optical depth of four through the default thickness.
    static constexpr foundation::float32 DEFAULT_EXTINCTION_PER_METRE = 0.004F;
    /// Default single-scattering albedo of water droplets with a small absorbing fraction.
    static constexpr foundation::float32 DEFAULT_SINGLE_SCATTERING_ALBEDO = 0.99F;
    /// Default Henyey-Greenstein asymmetry, a moderate forward peak.
    static constexpr foundation::float32 DEFAULT_PHASE_ASYMMETRY = 0.6F;
    /// Largest admitted SizeXMetres and SizeZMetres. The flat slab departs from the ground sphere by about
    /// 0.8 km at 100 km from the anchor, so larger layers need curved geometry.
    static constexpr foundation::float32 MAX_SIZE_METRES = 200000;
    /// Largest admitted magnitude of PhaseAsymmetry, which keeps the phase function's peak finite.
    static constexpr foundation::float32 MAX_PHASE_ASYMMETRY = 0.99F;

    /// False draws the clear sky. Other fields are ignored when false.
    bool IsEnabled = false;
    /// Height of the slab's base above the anchor's tangent plane, in metres. Finite and nonnegative.
    foundation::float32 BaseHeightMetres = DEFAULT_BASE_HEIGHT_METRES;
    /// Vertical extent in metres, finite and positive. The top must stay below the Atmosphere's top altitude.
    foundation::float32 ThicknessMetres = DEFAULT_THICKNESS_METRES;
    /// Full extent along the frame's X axis in metres, finite, positive and at most MAX_SIZE_METRES.
    foundation::float32 SizeXMetres = DEFAULT_SIZE_METRES;
    /// Full extent along the frame's Z axis in metres, finite, positive and at most MAX_SIZE_METRES.
    foundation::float32 SizeZMetres = DEFAULT_SIZE_METRES;
    /// Horizontal distance in metres over which density falls to zero at each side. Finite and nonnegative; zero
    /// draws sharp sides. Values above half a size are limited to it.
    foundation::float32 EdgeFadeMetres = DEFAULT_EDGE_FADE_METRES;
    /// Dimensionless multiplier of ExtinctionPerMetre, finite and nonnegative. Zero is an empty layer that
    /// reproduces the clear sky.
    foundation::float32 Density = 1;
    /// Extinction coefficient at unit density in inverse metres, finite and nonnegative.
    foundation::float32 ExtinctionPerMetre = DEFAULT_EXTINCTION_PER_METRE;
    /// Fraction of extinction that scatters, in [0, 1]. Zero is a pure absorber.
    foundation::float32 SingleScatteringAlbedo = DEFAULT_SINGLE_SCATTERING_ALBEDO;
    /// Henyey-Greenstein mean scattering cosine, at most MAX_PHASE_ASYMMETRY in magnitude. Zero is isotropic;
    /// positive values scatter forward, toward a view direction that looks at the Sun.
    foundation::float32 PhaseAsymmetry = DEFAULT_PHASE_ASYMMETRY;
    /// Nonzero finite world direction of the frame's +X axis. Only its part perpendicular to SurfaceUpWorld is
    /// used, which must be nonzero.
    foundation::math::Vector3 AxisXWorld{1, 0, 0};
    /// Optional two-region validation density and source. Disabled by default; see CloudLayerTestField.
    // Explicit default preserves designated-initializer callers under -Wmissing-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    CloudLayerTestField TestField{};
};

/// Procedural auroral curtains anchored to the Earth atmosphere's surface frame.
struct AuroraLayerDescription final
{
    /// Peak emission scale in scene-linear nits per metre for an ordinary display of northern lights.
    static constexpr foundation::float32 DEFAULT_EMISSION_PER_METRE = 0.0000005F;
    /// Largest admitted emission scale in nits per metre.
    static constexpr foundation::float32 MAXIMUM_EMISSION_PER_METRE = 0.001F;
    /// Duration of one seamless animation loop in seconds.
    static constexpr foundation::float32 LOOP_SECONDS = 120;

    /// False skips the layer and ignores its other settings.
    bool IsEnabled = false;
    /// Finite peak emission scale between zero and MAXIMUM_EMISSION_PER_METRE. Zero skips the layer.
    foundation::float32 EmissionPerMetre = DEFAULT_EMISSION_PER_METRE;
    /// Frozen animation time in seconds, in [0, LOOP_SECONDS]. Supply times from the app's simulation clock.
    foundation::float32 TimeSeconds = 0;
    /// Finite nonzero tangent X direction. The curtains run along X, toward the side X cross SurfaceUpWorld.
    /// Its part perpendicular to SurfaceUpWorld must be nonzero.
    foundation::math::Vector3 AxisXWorld{1, 0, 0};
};

/// Spherical Earth atmosphere resolved from one World entity and directional light.
struct AtmosphereDescription final
{
    /// False preserves the graph's authored background. Other fields are ignored when false.
    bool IsEnabled = false;
    /// Surface anchor in world metres. The ground sphere is tangent to this point.
    foundation::math::Vector3 SurfaceAnchorWorld{};
    /// Nonzero finite outward direction at the anchor, normalized during preparation.
    foundation::math::Vector3 SurfaceUpWorld{0, 1, 0};
    /// Nonzero finite direction toward the Sun, normalized during preparation.
    foundation::math::Vector3 DirectionToSunWorld{0, 1, 0};
    /// Finite nonnegative linear RGB illuminance in lux on a plane perpendicular to the Sun.
    foundation::math::Vector3 SolarIlluminance{};
    /// Earth profile's nominal solar radius in radians, from Hillaire's accompanying sample.
    static constexpr foundation::float32 DEFAULT_SOLAR_RADIUS_RADIANS = 0.004675F;
    /// Solar disk radius in radians, finite and strictly between zero and half pi.
    foundation::float32 SolarAngularRadiusRadians = DEFAULT_SOLAR_RADIUS_RADIANS;
    /// Stable source identity of the linked directional light in the scene. Zero supplies no direct light.
    foundation::uint64 SunSourceId = 0;
    /// Enable multiple scattering, skylight and aerial perspective for opaque deferred surfaces.
    /// Only applies when the view is admitted to the atmosphere shell.
    bool ShouldLightScene = false;
    /// Optional boundary provider. Its fallback lunar phase uses DirectionToSunWorld even in darkness.
    CelestialBackgroundDescription Celestial{};
    /// Optional cloud slab. The clustered deferred graph draws it for perspective cameras inside the shell, in the
    /// sky and in front of opaque surfaces. Orthographic and orbital views, ray kernels and reference captures omit
    /// it, and it casts no shadow on surfaces yet.
    CloudLayerDescription Clouds{};
    /// Optional view-ray emission between 100 and 300 km above the sphere. Applies to perspective cameras
    /// inside the clear-air shell on the native atmospheric deferred renderer. ShouldLightScene enables depth
    /// clipping at opaque surfaces beyond the emission base. Surface lighting and reference captures omit it.
    AuroraLayerDescription Aurora{};
};
/// Compute the signed altitude in metres above an Earth clear-sky surface.
/// Invalid anchor, up or eye coordinates preserve outAltitudeMetres.
[[nodiscard]] bool TryGetAtmosphereAltitude(const AtmosphereDescription& atmosphere,
                                            foundation::math::Vector3 eyeWorld,
                                            foundation::float64& outAltitudeMetres) noexcept;
/// Top boundary altitude in metres for the Earth clear-sky profile.
[[nodiscard]] foundation::float64 GetAtmosphereTopAltitude() noexcept;
/// Maximum background observer altitude in metres for the given projection and celestial settings.
[[nodiscard]] foundation::float64 GetAtmosphereMaximumViewAltitude(const AtmosphereDescription& atmosphere,
                                                                   bool isOrthographic) noexcept;

/// Largest side of a distant atmosphere image, matching EnvironmentDescription's source limit.
inline constexpr foundation::uint32 MAX_ATMOSPHERE_ENVIRONMENT_SIDE = 4096;

/// Outcome of ResolveAtmosphereEnvironment.
enum class AtmosphereEnvironmentStatus : foundation::uint8
{
    /// The sky image and the Sun's illuminance were written.
    Ready,
    /// The Atmosphere is disabled, does not light the scene, or the eye is below its ground or above its shell.
    /// The raster applies no atmosphere surface lighting in these cases either. Outputs are preserved.
    NotLit,
    /// Invalid atmosphere fields, eye position, image extent or output view. Outputs are preserved.
    InvalidDescription,
    /// The CPU tables could not be allocated. Outputs are preserved.
    OutOfMemory,
};

/// Resolve the distant lighting that an Earth clear-sky Atmosphere supplies to surfaces at eyeWorld.
///
/// Evaluates the same transmittance, multiple-scattering and sky tables as the raster atmosphere
/// (Hillaire 2020), on the CPU, at the eye's altitude. The sky image holds the in-scattered
/// radiance without the solar disk, which the linked Sun's analytic light supplies instead, and
/// without the night-sky boundary. Use it as a distant environment: aerial perspective between
/// surfaces and the eye is omitted, as is the altitude change across the scene.
/// @param atmosphere Validated as the render graph validates it. ShouldLightScene must be true.
/// @param eyeWorld Observer position in world metres, normally the camera position.
/// @param width Image width in texels, 1..MAX_ATMOSPHERE_ENVIRONMENT_SIDE.
/// @param height Image height in texels, 1..MAX_ATMOSPHERE_ENVIRONMENT_SIDE.
/// @param outPixels width*height linear RGB radiance texels in EnvironmentDescription's latitude/longitude layout
///        (+Y north, top row at the pole). Alpha is one.
/// @param outSunIlluminance Linear RGB illuminance of the linked Sun at the eye, after atmospheric transmittance.
///        Zero when the Sun is below the horizon.
/// @return Ready when both outputs were written; every other status preserves them.
/// @note Runs on the calling thread without device access. A 512x256 image takes tens of milliseconds.
[[nodiscard]] AtmosphereEnvironmentStatus
ResolveAtmosphereEnvironment(const AtmosphereDescription& atmosphere,
                             foundation::math::Vector3 eyeWorld,
                             foundation::uint32 width,
                             foundation::uint32 height,
                             foundation::BorrowedView<foundation::math::Vector4> outPixels,
                             foundation::math::Vector3& outSunIlluminance) noexcept;

/// @brief Measure the illuminance an Earth clear-sky Atmosphere delivers to level ground at its surface anchor.
///
/// Integrates the raster atmosphere's sky radiance (Hillaire 2020, the CPU tables ResolveAtmosphereEnvironment
/// uses) times the cosine to SurfaceUpWorld over the upper hemisphere, and adds the linked Sun's transmitted
/// illuminance at its elevation. The multiple-scattering term follows ShouldLightScene, as the displayed sky does.
/// The night-sky boundary, the Moon and other lights are not included. Pass the result, plus those sources, to
/// ComputeIncidentExposureValue100 for an incident-light camera exposure.
/// @param atmosphere Validated as the render graph validates it. ShouldLightScene may be false.
/// @param outIlluminanceLux Photometric illuminance in lux on a level plane, from the linear RGB luminance
///        (ITU-R BT.709 weights). Zero once the Sun is far enough below the horizon.
/// @return Ready when outIlluminanceLux was written. NotLit for a disabled Atmosphere; InvalidDescription and
///         OutOfMemory as for ResolveAtmosphereEnvironment. Every status but Ready preserves the output.
/// @note Runs on the calling thread without device access. Building the tables dominates the cost, a few
///       milliseconds; callers that meter every frame should cache the result per Atmosphere.
[[nodiscard]] AtmosphereEnvironmentStatus MeterAtmosphereIlluminance(const AtmosphereDescription& atmosphere,
                                                                     foundation::float64& outIlluminanceLux) noexcept;
} // namespace ludus::graphics::renderer
