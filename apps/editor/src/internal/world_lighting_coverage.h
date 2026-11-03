#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include "internal/builtin_world.h"

#include <QString>

namespace ludus::editor
{
struct RenderScene;

/// The light a World's extracted scene presents, summarised for DescribeWorldLighting. An imported scene can arrive
/// without usable lighting: the Intel Sponza glTF declares 24 punctual lights of zero intensity, which the importer
/// keeps disabled, and its only emitters are small bulbs (best-effort-scene-import.md, "Selecting a companion
/// lighting environment").
struct WorldLightingSummary final
{
    /// False when the World has no visible geometry or the viewport shows it unlit (RenderScene::Lit); BoundsWorld is
    /// then ignored.
    bool HasBounds = false;
    foundation::math::Aabb3 BoundsWorld{};
    /// Analytic lights the viewport presents: enabled imported lights not switched off, and authored light entities.
    foundation::int32 LightCount = 0;
    /// Imported lights stored disabled, for example authored with zero intensity.
    foundation::int32 DisabledLightCount = 0;
    /// Whether the scene has an environment with nonzero radiance.
    bool HasEnvironment = false;
    /// Enabled World panel luminaires (RenderScene::Luminaires).
    foundation::int32 LuminaireCount = 0;
    /// Visible objects whose surface emits.
    foundation::int32 EmissiveObjectCount = 0;
    /// Luminous flux of the emitting surfaces in lumens: pi x luminance x area per emitting side, with an emission map
    /// counted at its mean, or the requested power of a power-authored emitter.
    foundation::float64 EmittedLumens = 0;
    /// Direction the first disabled imported directional light shines along, when HasSunDirection. The importer keeps
    /// a zero-intensity light's authored direction, and a repair aims the sun the same way.
    bool HasSunDirection = false;
    foundation::math::Vector3 SunDirectionWorld{0, -1, 0};
    /// Whether the World binds the default light component, whose presets the repair creates.
    bool CanRepair = false;
};

/// The World edit that resolves a lighting warning. The editor offers it in a confirmation box, as one undoable edit.
enum class WorldLightingRepair : foundation::uint8
{
    None,   ///< Nothing to repair, or the World's schema has no light preset.
    AddSun, ///< Create a directional light entity at Sun.
};

/// Why the World renders dark, and how to fix it.
struct WorldLightingCoverage final
{
    /// Empty when the World presents enough light.
    QString Warning;
    WorldLightingRepair Repair = WorldLightingRepair::None;
    /// The sun AddSun creates.
    WorldSunPlacement Sun{};
};

/// Summarise scene for DescribeWorldLighting. Sums the area of emitting objects' triangles only, so it costs little
/// for a World with few emitters; call it when the World changes, not per frame.
[[nodiscard]] WorldLightingSummary SummarizeWorldLighting(const RenderScene& scene, bool canRepair);

/// Describe why world renders almost black, or return an empty Warning when it is lit. It is dark when it presents
/// no analytic light, environment or luminaire, and its emitters' flux spread over the surface of its bounds stays
/// below MIN_WORLD_MEAN_ILLUMINANCE_LUX. A World without bounds is never dark. With CanRepair, the result offers
/// FitWorldSun's sun.
[[nodiscard]] WorldLightingCoverage DescribeWorldLighting(const WorldLightingSummary& world);

/// The sun the World's Add Geometry / Light menu and its lighting repair create: DEFAULT_WORLD_SUN_ILLUMINANCE_LUX,
/// aimed like the first disabled imported directional light that shines down, or along
/// DEFAULT_WORLD_SUN_ROTATION_DEGREES, and placed above the center of the bounds (two metres above the origin
/// without them), where its gizmo is visible from a framing camera.
[[nodiscard]] WorldSunPlacement FitWorldSun(const WorldLightingSummary& world);

/// Euler rotation in degrees that aims a light's -Z along the unit directionWorld with no roll. The transform rotates
/// by Z, then Y, then X (render_scene.cpp), so -Z becomes (-cos x sin y, sin x, -cos x cos y).
[[nodiscard]] foundation::math::Vector3 ComputeWorldLightRotationDegrees(foundation::math::Vector3 directionWorld);

/// Mean illuminance, in lux, below which a World without lights reads as black. The estimate spreads the emitted flux
/// evenly over the surface of the World's bounds. At the viewport's exposure of one, a Lambertian surface of albedo
/// 0.5 under 0.01 lux has a radiance of 0.0016 nits, which encodes below 2% of sRGB white. Intel Sponza's bulbs give
/// about 0.001 lux.
inline constexpr foundation::float64 MIN_WORLD_MEAN_ILLUMINANCE_LUX = 0.01;
/// Rotation of a repair's sun when the scene has no directional light to follow: from 60 degrees above the horizon.
inline constexpr foundation::math::Vector3 DEFAULT_WORLD_SUN_ROTATION_DEGREES{-60, 30, 0};
/// Illuminance of a repair's sun in lux. Viewport exposure is one, so a surface of albedo 0.5 facing it has a radiance
/// of about 1.6 nits: bright without clipping the tone curve. Daylight is far brighter; this is a display choice.
inline constexpr foundation::float32 DEFAULT_WORLD_SUN_ILLUMINANCE_LUX = 10;
} // namespace ludus::editor
