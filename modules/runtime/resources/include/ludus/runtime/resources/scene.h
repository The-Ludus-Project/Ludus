#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/runtime/resources/manager.h>

namespace ludus::resources
{
/// CPU-owned RGBA8 sRGB mip zero. Rows are tightly packed, top to bottom.
struct SceneTexture final
{
    uint32 Width = 0;                ///< Positive width, at most 4096 texels.
    uint32 Height = 0;               ///< Positive height, at most 4096 texels.
    foundation::Array<uint8> Pixels; ///< Exactly Width*Height*4 bytes; at most 16 MiB.
};
/// Frozen RGB metallic/roughness reflection and authored emission profile.
struct SceneMaterial final
{
    uint32 Texture = ~uint32{0};          ///< Scene-local texture index; all ones means black untextured surface.
    bool Repeat = false;                  ///< Repeat UV addressing; false clamps at texture borders.
    float32 Metallic = 0;                 ///< Metallic fraction in [0,1].
    float32 Roughness = .5F;              ///< Perceptual roughness in [0.045,1], matching renderer admission.
    foundation::math::Vector3 Emission{}; ///< Linear RGB nits in v3; legacy scene-linear emission otherwise.
    bool IsEmissionTwoSided = true;       ///< Legacy v1/v2 defaults to two-sided; v3 uses its flags.
    bool HasEmissionPower = false;        ///< Resolve EmissionPower independently per placed Shape.
    uint64 AreaSourceId =
        0; ///< Stable emitter group ID; derived from material order in legacy bundles; zero for nonemitters.
    float32 EmissionPower = 0; ///< Total lumens after scale; zero without HasEmissionPower.
    /// Discard surface points whose base texture alpha is below AlphaCutoff (v5 ALPHA_MASKED).
    /// False before v5 and for opaque v5 materials, which ignore texture alpha.
    bool IsAlphaMasked = false;
    float32 AlphaCutoff = 0; ///< Masked threshold in [0,1]; zero when IsAlphaMasked is false.
};
/// Owned immutable CPU mesh arrays. Renderer installation copies these attributes.
struct SceneMesh final
{
    foundation::Array<foundation::math::Vector3> Positions; ///< Finite mesh-local positions.
    foundation::Array<foundation::math::Vector3> Normals;   ///< Finite nonzero authored shading normals.
    foundation::Array<foundation::math::Vector2> Texcoords; ///< Finite mip-zero texture coordinates.
    foundation::Array<foundation::math::Vector2>
        BakeTexcoords; ///< Stored v3 bake UVs; empty in v1/v2/v4, whose charts are implicit (see TryGetBakeChartUv).
    foundation::Array<uint32> Indices; ///< Triangle list, bounded by Positions count.
};
/// Transient resolved binding; persist resource IDs in the level, never these indices.
struct SceneObject final
{
    uint32 Mesh = 0;                     ///< Index into SceneDefinition::Meshes.
    uint32 Material = 0;                 ///< Index into SceneDefinition::Materials.
    uint64 SourceId = 0;                 ///< Stable nonzero instance ID; derived from object order in legacy bundles.
    foundation::math::Affine3 Transform; ///< Finite nonsingular mesh-local to world transform.
};
/// Versioned authored analytic light kind, independent of renderer GPU types.
enum class SceneLightKind : uint8
{
    Directional = 0, ///< IntensityRgb is illuminance in lux.
    Point = 1,       ///< IntensityRgb is luminous intensity in candela.
    Spot = 2,        ///< Candela with cosine cone attenuation.
};
/// CPU-owned authored light in the fixed Rec.709 photometric proxy.
struct SceneLight final
{
    uint64 SourceId = 0;                         ///< Stable nonzero identity unique in Lights.
    SceneLightKind Kind = SceneLightKind::Point; ///< Selects intensity units and attenuation.
    bool IsEnabled = true;                       ///< Disabled source keeps authoring values without transporting light.
    foundation::math::Vector3 IntensityRgb{};    ///< Nonnegative finite lux or candela, up to 1e6 per channel.
    foundation::math::Vector3 PositionWorld{};   ///< Metres.
    foundation::math::Vector3 DirectionWorld{0, -1, 0}; ///< Nonzero outgoing direction; not necessarily normalized.
    float32 Range = 100;                                ///< Positive transport/shadow range in metres, at most 1e6.
    float32 InnerCos = .9F;                             ///< Full-intensity cone cosine in (-1,1].
    float32 OuterCos = .8F;                             ///< Zero-intensity cone cosine in (-1,InnerCos).
};
/// Retained linear HDR source; workers decode without Qt, GPU calls or tone mapping.
struct SceneEnvironment final
{
    uint32 Width = 0;                ///< Equirectangular width; zero for constant radiance.
    uint32 Height = 0;               ///< Equirectangular height; zero for constant radiance.
    foundation::math::Vector3 Rgb{}; ///< RGB scale of Pixels, or constant nits when dimensions are zero.
    foundation::Array<foundation::math::Vector3> Pixels; ///< Width*Height finite nonnegative RGB nits, north-to-south.
};
/// Fully resident initial reference profile. Independent of Qt, source formats and GPU handles.
/// Decode on a worker, then publish immutable storage on the Manager control owner.
/// Runtime reconstruction uses exact project artifact revisions; no source parser is called.
struct SceneDefinition final
{
    // Counts are bounded by the bundle version's content::scene_format::GetBundleLimits:
    // the resident reference profile (versions 1-4) or the import profile (version 5).
    foundation::Array<SceneTexture> Textures;   ///< Textures; 32 MiB (v1-v4) or 256 MiB (v5) combined pixels.
    foundation::Array<SceneMaterial> Materials; ///< Distinct materials; at most 8 (v1-v4) or 256 (v5).
    foundation::Array<SceneMesh> Meshes;        ///< Distinct meshes; at most 6 (v1-v4) or 256 (v5).
    foundation::Array<SceneObject> Objects;     ///< Independent instances; at most 128 (v1-v4) or 1024 (v5).
    foundation::Array<SceneLight> Lights;       ///< At most 128 analytic sources; empty for legacy bundles.
    SceneEnvironment Environment;               ///< Authored environment; black for legacy bundles.
    foundation::math::Vector3 Eye;              ///< Authored perspective pinhole in world units.
    foundation::math::Vector3 Target;           ///< Finite look-at target distinct from Eye.
    foundation::math::Vector3 Up;               ///< Camera up direction not parallel to its forward direction.
    float32 VerticalFov = 1;                    ///< Perspective vertical field of view in radians, in (0,pi).
    /// Decode LUSCN01 versions one through five little-endian interchange, at most 64 MiB for versions one to
    /// four and 384 MiB for version five (BundleLimits::BundleBytes). No file I/O or source
    /// import. Failure preserves output; allocation failures return OutOfMemory. The caller owns all resulting arrays
    /// and must keep them immutable while any adapter/lease references them.
    [[nodiscard]] static Status Decode(const uint8* bytesOrNull, usize count, SceneDefinition& outScene) noexcept;
    /// Known owned array bytes, excluding allocator metadata and driver allocations.
    [[nodiscard]] uint64 Bytes() const noexcept;
};
/// Aggregate CPU scene adapter for the fully resident profile; GPU ownership is separate.
/// Uses one prevalidated immutable definition borrowed from its composition owner. The owner
/// keeps it alive through Manager::Shutdown returning Ok. No I/O, decoder, allocation or waits
/// occur in callbacks. The renderer copies geometry/textures and owns submitted-use retirement.
/// This aggregate is a transient render binding, not the project catalog's per-asset graph.
class SceneDefinitionAdapter final
{
public:
    /// Construct an unbound context without allocation.
    SceneDefinitionAdapter() noexcept = default;
    /// Release control storage. @pre Every manager using Descriptor has drained Shutdown (Ok).
    ~SceneDefinitionAdapter() noexcept;
    /// Context identities cannot be copied.
    SceneDefinitionAdapter(const SceneDefinitionAdapter&) = delete;
    /// Context identities cannot be assigned.
    SceneDefinitionAdapter& operator=(const SceneDefinitionAdapter&) = delete;
    /// Bind a predecoded scene and an exact derived snapshot ID. An initialized adapter
    /// cannot be rebound. @pre scene is an unchanged successful Decode result. Type must be CookedScene; invalid input
    /// preserves the adapter.
    [[nodiscard]] Status Initialize(const content::Resource& resource, const SceneDefinition& scene) noexcept;
    /// Copied callback table; CPU bytes remain composition-owned, outside manager reservations.
    /// @pre Initialize succeeded; adapter and scene outlive the manager and its leases.
    [[nodiscard]] Adapter Descriptor() noexcept;

private:
    struct State;
    State* mState = nullptr;
    static uint64 Epoch(void* context) noexcept;
    static Status Begin(void* context, const content::Resource& resource, void*& candidate) noexcept;
    static Status Poll(void* context, void* candidate, const void*& payload) noexcept;
    static void Cancel(void* context, void* candidate) noexcept;
    static Status Retire(void* context, void* candidate) noexcept;
    static Status Validate(void* context, const void* payload) noexcept;
};
} // namespace ludus::resources
