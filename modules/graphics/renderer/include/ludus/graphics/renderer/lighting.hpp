#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/lighting_layout.h>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
namespace internal
{
struct Access;
}
/// Immutable caster snapshot, light projections and rendered RGBA8 depth atlas.
/// Release detaches ownership; lit materials retain this exact incarnation.
struct ShadowAtlas final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Grants private access to the renderer incarnation.
    friend struct internal::Access;
};
/// Resolve RGB emission color and total lumens to nits for one placed emitter.
/// Uses the Renderer Rec.709 photometric proxy and world area in square metres;
/// two-sided emitters split total power across both sides. Finite nonnegative color,
/// power and positive area are required. Invalid/out-of-profile values preserve outEmission.
[[nodiscard]] rhi::RasterStatus ResolveMeshEmissionPower(foundation::math::Vector3 color,
                                                         foundation::float32 lumens,
                                                         foundation::float64 areaWorld,
                                                         bool isTwoSided,
                                                         foundation::math::Vector3& outEmission) noexcept;
/// Number of corners in a triangle emitter.
inline constexpr ludus::foundation::usize AREA_TRIANGLE_CORNERS = 3;
/// Bounded deterministic quadrature, trading near-field accuracy for raster cost.
enum class AreaLightQuadrature : ludus::foundation::uint8
{
    Centroid,   ///< One sample at the centroid; coarse near-field approximation.
    ThreePoint, ///< Three symmetric barycentric samples, each with area/3 weight.
};
/// Finite world-space Lambertian triangle emitter; power must be resolved first.
struct TriangleAreaLightDescription final
{
    ludus::foundation::StaticArray<ludus::foundation::math::Vector3, AREA_TRIANGLE_CORNERS>
        PositionsWorld{};                        ///< Winding defines outward emitting normal; metres.
    ludus::foundation::math::Vector3 Emission{}; ///< Nonnegative RGB nits, each channel at most 10000.
    ludus::foundation::uint64 FirstSampleSourceId =
        0; ///< Nonzero base ID; consecutive IDs must fit uint64 and be unique in the final light list.
    ludus::foundation::float32 Range = 100; ///< Smooth transport/shadow range in metres, [.01,1e6].
    bool IsTwoSided = false;                ///< Whether both geometric sides emit.
    AreaLightQuadrature Quadrature = AreaLightQuadrature::Centroid; ///< Declared integration approximation.
};
/// Owned fixed-capacity output; no allocation, retained pointers or GPU resources.
struct AreaLightSamples final
{
    ludus::foundation::StaticArray<DirectLight, AREA_TRIANGLE_CORNERS>
        Lights{};                        ///< Valid prefix of quadrature samples.
    ludus::foundation::uint32 Count = 0; ///< One or three samples on success.
};
/// Expand a nondegenerate finite triangle into weighted Lambertian samples.
/// Returns CapacityExceeded if radiance*sample area exceeds the direct-light
/// intensity budget; malformed inputs return InvalidDescription. All failures
/// preserve outSamples. This quadrature does not reproduce finite-source soft
/// shadows exactly; each sample needs six cube shadow faces for visibility.
[[nodiscard]] rhi::RasterStatus BuildTriangleAreaLights(const TriangleAreaLightDescription& description,
                                                        AreaLightSamples& outSamples) noexcept;
/// One connected planar source patch; triangle tessellation does not change its
/// area-weighted centroid or emitted power. Borrowed inputs are consumed synchronously.
struct PlanarAreaLightDescription final
{
    ludus::foundation::BorrowedView<const ludus::foundation::math::Vector3>
        PositionsMesh; ///< Finite mesh-local positions; borrowed for this call.
    ludus::foundation::BorrowedView<const ludus::foundation::uint32>
        Indices; ///< Triangle list with consistent outward winding; producer guarantees one connected patch.
    ludus::foundation::math::Affine3
        MeshToWorld{};                           ///< Finite nonsingular transform; areas are computed in world metres.
    ludus::foundation::math::Vector3 Emission{}; ///< Resolved RGB nits, each channel in [0,10000].
    ludus::foundation::uint64 SourceId = 0;      ///< Unique nonzero proxy identity.
    ludus::foundation::float32 Range = 100;      ///< Smooth attenuation/shadow range in metres.
    bool IsTwoSided = false;                     ///< Emit from both geometric sides.
};
/// Construct one tessellation-independent centroid quadrature sample. Validate
/// bounds, nondegeneracy, consistent normals and coplanarity within 0.1 mm plus
/// FP32 coordinate error. Centroid must lie inside one supplied triangle;
/// unsupported curved/disjoint/concave-centroid sources preserve outLight.
/// Connectivity is a producer contract, not a topology proof. Near-field light
/// shape and soft shadows are approximated; report this declared quality profile.
[[nodiscard]] rhi::RasterStatus BuildPlanarAreaLight(const PlanarAreaLightDescription& description,
                                                     DirectLight& outLight) noexcept;
/// Cube projection count per point or area sample.
inline constexpr ludus::foundation::uint32 POINT_SHADOW_FACES = 6;
/// Build a conventional-depth 90-degree point-light projection, faces in order
/// +X,-X,+Y,-Y,+Z,-Z. Distances are metres, with 0 < nearDistance < farDistance.
/// Invalid/nonrepresentable input preserves outWorldToClip; no allocation.
[[nodiscard]] rhi::RasterStatus BuildPointShadowMatrix(ludus::foundation::math::Vector3 positionWorld,
                                                       ludus::foundation::uint32 face,
                                                       ludus::foundation::float32 nearDistance,
                                                       ludus::foundation::float32 farDistance,
                                                       ludus::foundation::math::Matrix4& outWorldToClip) noexcept;
/// Validate up to 128 lights and retain the eight smallest unique SourceIds.
/// No allocation; invalid/count overflow preserves output. Saturation succeeds with Dropped.
[[nodiscard]] rhi::RasterStatus
SelectDirectLights(const DirectLight* lights, ludus::foundation::usize count, LightSelection& output) noexcept;
/// Validate/normalize and copy every one of at most 128 unique lights in SourceId
/// order. No saturation or allocation; failure preserves output. Used by explicit
/// reference transport, where silently dropping illumination would change the integral.
[[nodiscard]] rhi::RasterStatus
NormalizeDirectLights(const DirectLight* lights, ludus::foundation::usize count, LightSelection& output) noexcept;
/// Regular 8x8x8 grid in a rigidly transformed space, independent of opaque depth.
struct LightClusterDescription final
{
    /// World-to-grid rigid transform; columns must be orthonormal within 1e-5.
    ludus::foundation::math::Affine3 WorldToGrid{};
    /// Finite grid-space bounds in meters, each extent [0.01,2000000].
    /// Surfaces outside this box evaluate the full admitted light list.
    ludus::foundation::math::Aabb3 Bounds{{-10, -10, -10}, {10, 10, 10}};
};
/// Inspectable CPU assignment, copied on success; no resource ownership or allocation.
struct LightClusters final
{
    /// All admitted normalized lights in ascending SourceId order, at most 128.
    DirectLight Lights[MAX_DIRECT_LIGHTS]{};
    /// Initialized light count; assignment never drops admitted lights.
    ludus::foundation::uint32 Count = 0;
    /// Cell counts, capped at 32; x varies fastest, then y, then z.
    ludus::foundation::uint8 Counts[512]{};
    /// Cells with more than 32 intersections evaluate the full list in the shader.
    bool Overflow[512]{};
    /// Fixed-stride indices plus one; zero terminates. Overflow starts with 255.
    /// Uploaded as a nearest-sampled 64x64 RGBA8 texture (32 bytes per cell).
    ludus::foundation::uint8 Texels[16384]{};
    /// Number of overflow cells, explicitly reported rather than dropping lights.
    ludus::foundation::uint32 OverflowCells = 0;
    /// Written cell/light intersections, at most 512*32.
    ludus::foundation::uint32 Assignments = 0;
};
/// Validate and normalize up to 128 unique lights, then assign conservative sphere/AABB lists.
/// Directional lights intersect every cell; spot cones use their enclosing range sphere.
/// No occupied-depth or normal rejection. Failure preserves output; no allocation.
[[nodiscard]] rhi::RasterStatus BuildLightClusters(const LightClusterDescription& description,
                                                   const DirectLight* lights,
                                                   ludus::foundation::usize count,
                                                   LightClusters& output) noexcept;
/// Metadata accompanying one baked irradiance chart; no runtime bake or chart packing.
struct BakedLighting final
{
    /// Ready single-mip linear RGBA8 chart with replicated gutters; null declares environment fallback.
    rhi::TextureHandle Texture{};
    /// Linear irradiance multiplier in [0,10000]; texture stores normalized linear RGB.
    ludus::foundation::math::Vector3 Scale{1, 1, 1};
    /// Bake UV scale XY and offset ZW, selecting at least two interior texel centers per axis.
    /// Bounds must leave a replicated gutter texel plus half a texel on every side (1e-6 UV rounding tolerance).
    ludus::foundation::math::Vector4 UvTransform{1, 1, 0, 0};
    /// True selects independent guttered triangle charts from SceneFormat helpers,
    /// with power-of-two dimensions up to 4096, identity UvTransform and no mipmaps. False retains the legacy
    /// subrectangle transform admission. The producer must replicate each chart's gutter texels.
    bool AreTriangleChartsEnabled = false;
    /// True declares static area direct diffuse in this chart; do not also submit
    /// raster area samples for its diffuse term. Analytic direct stays raster.
    bool HasStaticAreaDirect = false;
    /// Exact nonzero source identity and authoring revisions required for bake validity.
    ludus::foundation::uint64 SourceId = 0;
    /// Scene bake revision; SceneItem must supply the same value.
    ludus::foundation::uint64 SceneRevision = 0;
    /// Cook dependency revision/hash; SceneItem must supply the same value.
    ludus::foundation::uint64 DependencyRevision = 0;
    /// Exact local-to-world used by the bake; moved objects use environment fallback.
    ludus::foundation::math::Affine3 Transform{};
    /// Optional baked direct-light identity; replace that light's diffuse term only for valid bakes.
    /// Zero replaces no analytic direct term; HasStaticAreaDirect separately declares area diffuse. One chart replaces
    /// at most one direct diffuse term; dynamic direct specular remains.
    ludus::foundation::uint64 DirectLightId = 0;
};
/// Diagnostic output selected by the debug-view lit recipes (`renderer_lit_debug`,
/// `renderer_clustered_authored_debug`). Ordinary lit programs ignore it and shade normally.
/// Debug colours are pre-compensated for RasterViewport's Reinhard + sRGB presentation at
/// exposure one, so they display as described; RGBA8 scene targets clip values above 0.73.
enum class MaterialDebugView : ludus::foundation::uint8
{
    /// Ordinary lit shading.
    None = MATERIAL_DEBUG_VIEW_NONE,
    /// Shaded world-space normal mapped from [-1,1] to [0,1] RGB.
    NormalWorld = MATERIAL_DEBUG_VIEW_NORMAL_WORLD,
    /// Camera distance mapped linearly over ViewDescription::DebugDepthRange, near black to far white.
    LinearDepth = MATERIAL_DEBUG_VIEW_LINEAR_DEPTH,
    /// Perceptual roughness after surface-map scaling, as grey.
    Roughness = MATERIAL_DEBUG_VIEW_ROUGHNESS,
    /// Metallic fraction after surface-map scaling, as grey.
    Metallic = MATERIAL_DEBUG_VIEW_METALLIC,
    /// Fractional mesh UV0 as red (U) and green (V).
    TexcoordUv = MATERIAL_DEBUG_VIEW_TEXCOORD_UV,
    /// Direct lights with a nonzero shadowed contribution at the pixel, as a blue-to-red heat
    /// ramp saturating at MATERIAL_DEBUG_LIGHT_COUNT_SATURATION; black means no direct light.
    LightCount = MATERIAL_DEBUG_VIEW_LIGHT_COUNT,
};
/// Immutable unlit base schema plus the L4 metallic/roughness lighting terms.
struct LitMaterialDescription final
{
    /// Base texture, tint, alpha and sampler retain the L3 contracts.
    MaterialDescription Surface{};
    /// Metallic fraction in [0,1]; dielectric F0 is fixed at 0.04.
    ludus::foundation::float32 Metallic = 0;
    /// Perceptual roughness in [0.045,1]; alpha is its square.
    ludus::foundation::float32 Roughness = 0.5F;
    /// Optional ready sampled Rgba8Unorm surface map: RG encodes tangent normal XY
    /// as 2*channel-1; positive Z is reconstructed. B multiplies perceptual roughness
    /// (floored at 0.045), A multiplies metallic. It shares Surface UV/sampler/LOD.
    /// Requires renderer_surface.slang or renderer_clustered_surface.slang; other
    /// programs return Unsupported. Null preserves the constant surface model.
    /// Creation retains the exact view; the caller may destroy its texture owner.
    /// Reference captures require retained CPU bytes for the selected capture mip.
    rhi::TextureHandle SurfaceMap{};
    /// Tangent normal XY strength [0,8]; zero disables perturbation, preserving
    /// roughness/metallic sampling. Degenerate UV frames preserve the mesh normal.
    ludus::foundation::float32 NormalScale = 1;
    /// Opt-in authored tangent/color/UV profile; requires an authored shader recipe.
    /// False preserves legacy geometry semantics. Authored normals are not regenerated.
    bool AreAuthoredInputsEnabled = false;
    /// Base/surface-map material UV channel, 0 or 1. Both maps share the transform.
    /// UV1 requires AreAuthoredInputsEnabled; independently transformed maps must be cooked.
    ludus::foundation::uint32 MaterialUvSet = 0;
    /// Optional ready HDR map on the same device, retained during creation.
    /// Requires an authored recipe. Its SH/prefiltered specular replace the constant
    /// environment terms; baked diffuse still takes precedence. Null uses constants.
    const EnvironmentMap* EnvironmentOrNull = nullptr;
    /// Linear surface emission in [0,10000]; does not create a scene light.
    ludus::foundation::math::Vector3 Emission{};
    /// Legacy default is two-sided emission. False emits only from front-facing geometry;
    /// reference transport evaluates the triangle's geometric winding.
    bool IsEmissionTwoSided = true;
    /// Optional ready RGBA8 linear/sRGB RGB multiplier for Emission; alpha ignored.
    /// Authored recipes only. Shares the selected material UV transform and sampler.
    /// Creation retains its exact view; reference requires CPU bytes at TextureMip.
    rhi::TextureHandle EmissionMap{};
    /// Constant diffuse irradiance in [0,10000], used when a baked chart is absent/invalid.
    ludus::foundation::math::Vector3 EnvironmentIrradiance{};
    /// Hemispheric variation of the constant EnvironmentIrradiance in [0,1]: it is the irradiance of a surface
    /// facing +Y (the sky), and a surface facing -Y receives that much times this scale, blended by the
    /// normal's Y. One keeps the irradiance constant, as before. Ignored when baked or authored irradiance applies.
    ludus::foundation::float32 EnvironmentGroundScale = 1;
    /// Constant filtered environment radiance in [0,10000]; single-scatter Fresnel approximation.
    ludus::foundation::math::Vector3 EnvironmentRadiance{};
    /// Borrowed list, validated and copied before CreateLitMaterial returns.
    const DirectLight* Lights = nullptr;
    /// At most 128 input lights; L4 retains eight, L5 clusters retain all.
    ludus::foundation::usize LightCount = 0;
    /// Optional immutable bake chart/version with explicit direct-light replacement.
    BakedLighting Bake{};
    /// Optional L5 assignment profile; requires a clustered shader program. Copied during creation.
    /// Null retains the L4 eight-light selection. Lights/LightCount supply either profile.
    const LightClusterDescription* Clusters = nullptr;
    /// Ready rendered atlas; null supplies unshadowed direct lighting.
    ShadowAtlas Shadows{};
    /// Diagnostic output for debug-view programs; ignored by ordinary lit programs.
    /// Values outside the enumeration are rejected.
    MaterialDebugView DebugView = MaterialDebugView::None;
};
/// Decoded linear surface-map parameters, with a unit positive-hemisphere tangent normal.
struct SurfaceParameters final
{
    /// Unit tangent-space direction; positive Y follows increasing transformed texture V.
    ludus::foundation::math::Vector3 NormalTangent{0, 0, 1};
    /// Texture-modulated metallic in [0,1].
    ludus::foundation::float32 Metallic = 0;
    /// Texture-modulated perceptual roughness in [0.045,1].
    ludus::foundation::float32 Roughness = 0.5F;
};
/// Decode a filtered linear RGBA surface-map texel without allocation.
/// Texel channels/metallic must be [0,1], roughness [0.045,1], strength [0,8].
/// RG outside the unit disk is normalized to its rim before Z reconstruction;
/// strength scales XY after reconstruction and the direction is normalized again.
/// Invalid inputs return InvalidDescription and preserve outParameters.
[[nodiscard]] rhi::RasterStatus DecodeSurfaceParameters(ludus::foundation::math::Vector4 texel,
                                                        ludus::foundation::float32 metallic,
                                                        ludus::foundation::float32 roughness,
                                                        ludus::foundation::float32 normalScale,
                                                        SurfaceParameters& outParameters) noexcept;
/// One atlas cell, authored from an unjittered light view; order selects overlapping cascades.
struct ShadowRegion final
{
    /// Exact light description. Point/area samples require all six canonical cube faces.
    DirectLight Light{};
    /// Cube face 0..5 for point/area; all ones for directional/spot projections.
    ludus::foundation::uint32 CubeFace = ~ludus::foundation::uint32{0};
    /// Cube near distance in metres; canonical far distance is Light.Range.
    ludus::foundation::float32 NearDistance = .01F;
    /// Canonical conventional-depth light projection, Z [0,1], XY [-1,1], Y up.
    ludus::foundation::math::Matrix4 WorldToClip = ludus::foundation::math::Matrix4::Identity();
    /// Inner drawing rectangle; must leave at least one texel of clear gutter on every side.
    rhi::RasterRectangle Interior{};
    /// Receiver bias in normalized light depth, [0,0.01], independent of camera reverse-Z.
    ludus::foundation::float32 Bias = 0.001F;
};
/// Bounded 80-region atlas; thirteen point/area samples with all six cube faces.
struct ShadowAtlasDescription final
{
    /// RGBA8 atlas width, [4,2048], subject to device limits.
    ludus::foundation::uint32 Width = 256;
    /// RGBA8 atlas height, [4,2048], subject to device limits.
    ludus::foundation::uint32 Height = 256;
    /// Borrowed disjoint guttered regions; copied during preparation.
    const ShadowRegion* Regions = nullptr;
    /// Positive count, at most SHADOW_REGION_CAPACITY; cascades may share one directional light identity.
    ludus::foundation::usize Count = 0;
};
/// Fit a stabilized directional light projection to finite receiver/caster bounds.
/// Snaps XY center to the inner-resolution texel grid, pads one texel and uses conventional
/// depth. Call with unjittered cascade bounds; no camera split policy is hidden here.
/// Invalid/degenerate/nonrepresentable input preserves output. Allocation-free.
[[nodiscard]] rhi::RasterStatus BuildDirectionalShadowMatrix(const ludus::foundation::math::Aabb3& bounds,
                                                             ludus::foundation::math::Vector3 direction,
                                                             ludus::foundation::uint32 resolution,
                                                             ludus::foundation::math::Matrix4& output) noexcept;
/// Display composition parameters for one immutable presentation version.
struct LightingOutput final
{
    /// Manual scene exposure multiplier, [0.000001,1000], applied exactly once.
    ludus::foundation::float32 Exposure = 1;
    /// Apply guarded per-channel Reinhard after exposure; false preserves linear RGB.
    bool ToneMap = true;
    /// Encode to sRGB for unorm display textures/surfaces; hardware sRGB surfaces encode instead.
    /// False selects a linear display intermediate, suitable for subsequent linear UI blending.
    bool Encode = true;
};
/// Independent CPU reference for the documented single-scatter GGX/Smith/Schlick + Lambert BRDF.
/// Base color and metallic [0,1], roughness [0.045,1]; directions normalized on admission.
/// Returns the BRDF times max(N dot L,0), without incident radiance; failure preserves output.
[[nodiscard]] rhi::RasterStatus EvaluateLitBrdf(ludus::foundation::math::Vector3 baseColor,
                                                ludus::foundation::float32 metallic,
                                                ludus::foundation::float32 roughness,
                                                ludus::foundation::math::Vector3 normal,
                                                ludus::foundation::math::Vector3 view,
                                                ludus::foundation::math::Vector3 light,
                                                ludus::foundation::math::Vector3& output) noexcept;
} // namespace ludus::graphics::renderer
