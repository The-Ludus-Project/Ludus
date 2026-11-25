#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

/// @file
/// World features: optional parts of a World that a render graph may draw. Ray-capable features map to
/// engine-owned scene-interface functions. The graph builder records which of those functions the compiled kernels
/// call, so a graph's support follows its code, not a manifest flag. This header owns every feature's name, function,
/// editor text and warning code; the "World features" section of docs/development/render-graph-authoring.md shows the
/// same table.
namespace ludus::graphics::renderer
{
/// An optional World feature a render graph can draw. Values are stable indices into the editor's table.
enum class WorldFeature : foundation::uint8
{
    /// The Atmosphere's sky as the background and distant lighting, and its Sun dimmed by the air.
    AtmosphereSky,
    /// Haze between the camera and surfaces: in-scattered sky light and transmittance along the view ray.
    AerialPerspective,
    /// Auroral curtains in the native clustered deferred atmosphere.
    Aurora,
};
/// Number of WorldFeature values.
inline constexpr foundation::usize WORLD_FEATURE_COUNT = 3;
/// One bit per WorldFeature, bit index equal to the enum value.
using WorldFeatureMask = foundation::uint32;

/// Return the WorldFeatureMask bit of a feature.
[[nodiscard]] LUDUS_INLINE constexpr WorldFeatureMask GetWorldFeatureBit(WorldFeature feature) noexcept
{
    return WorldFeatureMask{1} << static_cast<foundation::uint32>(feature);
}

/// How much of a World feature one render graph draws.
enum class WorldFeatureSupport : foundation::uint8
{
    /// The graph draws the World without this feature.
    NotRendered,
    /// A ray kernel calls the feature's scene function, which evaluates the documented ray approximation.
    Approximated,
    /// The graph composes the feature as the engine's default raster graph does.
    Supported,
};

/// Static UTF-8 text describing one World feature to graph authors. Every field is a complete sentence or name.
struct WorldFeatureDescription final
{
    /// Short name for tables and warnings, such as "Atmosphere sky".
    const char* Name = "";
    /// Scene-interface function a ray kernel calls to draw the feature, with its parameters. Empty for raster-only
    /// features.
    const char* Function = "";
    /// Where in a graph's callbacks to call Function, or how to view a raster-only feature.
    const char* Callback = "";
    /// What the viewport shows instead while the feature is not rendered.
    const char* Omission = "";
    /// Limits of the ray approximation that Function evaluates, compared with the default raster graph.
    const char* Approximation = "";
};

/// Code of the editor warning that names World features the drawing graph does not render.
inline constexpr const char* WORLD_FEATURE_WARNING_CODE = "RenderGraphFeatureNotRendered";

/// Describe one World feature. Header-inline so that editor components without the renderer library share the text.
/// @param feature A WorldFeature value.
/// @return Pointers to static text; empty strings for a value outside the enum.
[[nodiscard]] LUDUS_INLINE constexpr WorldFeatureDescription DescribeWorldFeature(WorldFeature feature) noexcept
{
    // Keep docs/development/render-graph-authoring.md#world-features in step; world_feature_tests.cpp compares them.
    constexpr WorldFeatureDescription ATMOSPHERE_SKY
    {
        .Name = "Atmosphere sky",
        .Function = "EvaluateEnvironment(scene, directionWorld)",
        .Callback = "Return it where a ray leaves the scene: in the miss callback, or where ray generation finds no "
                    "surface. EvaluateLightAlongRay, SampleLight and FollowSpecularChain call it for you.",
        .Omission = "The background is the World environment, and the Atmosphere's Sun is not dimmed by the air.",
        .Approximation = "A distant sky resolved at the camera's altitude when the scene is prepared, without the "
                         "night sky, replaces the World environment; the linked Sun keeps the illuminance that "
                         "reaches the camera.",
    };
    constexpr WorldFeatureDescription AERIAL_PERSPECTIVE
    {
        .Name = "Aerial perspective",
        .Function = "ApplyAerialPerspective(scene, originWorld, hitWorld, radiance)",
        .Callback = "Call it on the radiance a camera ray brings back from a surface, in the closest-hit callback or "
                    "in ray generation after shading, with the ray origin and the hit position.",
        .Omission = "Distant surfaces are neither hazed nor dimmed by the air.",
        .Approximation = "Every segment uses the haze seen from the camera's altitude, read from a table resolved "
                         "when the scene is prepared, up to 32 km.",
    };
    constexpr WorldFeatureDescription AURORA
    {
        .Name = "Northern lights",
        .Callback =
            "Use the native clustered deferred raster graph with a perspective camera inside the clear-air shell.",
        .Omission = "The sky has no auroral curtains.",
    };
    switch (feature)
    {
        case WorldFeature::AtmosphereSky:
            return ATMOSPHERE_SKY;
        case WorldFeature::AerialPerspective:
            return AERIAL_PERSPECTIVE;
        case WorldFeature::Aurora:
            return AURORA;
    }
    return {};
}
/// Name a support level for tables: "Supported", "Approximated" or "Not rendered".
/// @return Static UTF-8 text; empty for a value outside the enum.
[[nodiscard]] LUDUS_INLINE constexpr const char* DescribeWorldFeatureSupport(WorldFeatureSupport support) noexcept
{
    switch (support)
    {
        case WorldFeatureSupport::NotRendered:
            return "Not rendered";
        case WorldFeatureSupport::Approximated:
            return "Approximated";
        case WorldFeatureSupport::Supported:
            return "Supported";
    }
    return "";
}
} // namespace ludus::graphics::renderer
