#pragma once

// Light authoring data and the selected light list, split from lighting.hpp so
// code that only stores lights does not parse the lighting and raster APIs.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
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
    ludus::foundation::float32 Range = 10;
    /// Spot inner half-angle cosine; -1 < OuterCos < InnerCos <= 1.
    ludus::foundation::float32 InnerCos = 0.9F;
    /// Spot outer half-angle cosine, distinct from InnerCos.
    ludus::foundation::float32 OuterCos = 0.8F;
};
/// Stable bounded forward list; entries sorted by SourceId, not brightness or opaque depth.
struct LightSelection final
{
    /// L4 selects at most eight; L5 clustered materials report all admitted lights (up to 128).
    DirectLight Lights[128]{};
    /// Number of initialized entries in Lights.
    ludus::foundation::uint32 Count = 0;
    /// Submitted count minus Count; positive means a declared quality reduction.
    ludus::foundation::uint32 Dropped = 0;
    /// L5 cells using full-list fallback; zero for L4 selection.
    ludus::foundation::uint32 OverflowCells = 0;
    /// L5 written cell/light entries; zero for L4 selection.
    ludus::foundation::uint32 Assignments = 0;
};
} // namespace ludus::graphics::renderer
