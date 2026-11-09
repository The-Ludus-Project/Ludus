#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/runtime/resources/scene.h>

#include <QImage>
#include <QJsonObject>
#include <QVector>

namespace ludus::graphics::renderer
{
struct ViewDescription;
} // namespace ludus::graphics::renderer

namespace ludus::editor
{
class WorldDocument;

/// Where a World's render extraction comes from; one World has at most one source.
enum class RenderSceneSource : foundation::uint8
{
    /// Nothing renders the World yet: no project bindings and no game adapter.
    None,
    /// The project's authoring/render.preview.json, or bundled default-World bindings, supplies renderables.
    ProjectBindings,
    /// The World-demo game extracts its own frame (apps/world_demo render_items).
    WorldDemo,
};
// Private extraction adapter: project bindings name existing World properties.
// Load validates every file before replacing output; no game code is executed.
struct PreviewMesh final
{
    QString Id;
    QVector<foundation::math::Vector3> Positions;
    QVector<foundation::math::Vector2> Uvs;
    QVector<foundation::math::Vector3> Normals;
    QVector<foundation::uint32> Indices;
};
struct PreviewTexture final
{
    QString Id;
    QImage Pixels;
    /// Linear data (a cooked v6 surface map), uploaded as RGBA8 unorm rather than sRGB.
    bool IsLinear = false;
};
/// Editor-only tint for a surface whose imported material needs a mapping.
inline constexpr foundation::math::Vector4 UNRESOLVED_MATERIAL_TINT{1, 0, 1, 1};

struct PreviewSurface final
{
    bool HasUnresolvedMaterial = false;
    foundation::int32 Texture = -1;
    foundation::math::Vector4 Tint{1, 1, 1, 1};
    /// Straight linear RGBA base color for consumers that sample no textures, such as the
    /// render-graph RendererStatic preview. Authored surfaces use Tint. Cooked surfaces
    /// compute it once at decode: Tint times the linear mean of the base-color texture's mip-zero
    /// RGB, with alpha one because a consumer that samples no texture cannot evaluate an alpha mask.
    /// Untextured cooked surfaces keep their black Tint. The lit and textured paths ignore it.
    foundation::math::Vector4 FlatTint{1, 1, 1, 1};
    bool Repeat = false;
    foundation::float32 Metallic = 0;
    foundation::float32 Roughness = .5F;
    content::scene_format::MaterialScattering Scattering{};
    foundation::math::Vector3 Emission{};
    bool IsEmissionTwoSided = false;
    bool HasEmissionPower = false;
    foundation::float32 EmissionPower = 0;
    /// Discard where the base texture's alpha is below AlphaCutoff (cooked LUSCN v5 masks);
    /// opaque surfaces ignore texture alpha.
    bool IsAlphaMasked = false;
    foundation::float32 AlphaCutoff = 0;
    /// Cooked v6 linear surface map (RG tangent normal, B roughness, A metallic), or -1.
    foundation::int32 SurfaceTexture = -1;
    /// Cooked v6 sRGB emission colour map multiplying Emission, or -1.
    foundation::int32 EmissionTexture = -1;
    /// Tangent-normal strength of SurfaceTexture; zero without a normal map.
    foundation::float32 NormalScale = 0;
    /// Linear mean RGB of EmissionTexture (one without a map): scales the area-light proxy
    /// of a texture-modulated emitter, which emits that mean on average.
    foundation::math::Vector3 EmissionMapMean{1, 1, 1};
};
struct PreviewObject final
{
    QString Id;
    QString Name;
    foundation::int32 Mesh = 0;
    foundation::int32 Texture = -1;
    foundation::int32 Surface = -1;
    foundation::math::Affine3 Transform;
    foundation::math::Vector4 Tint{1, 1, 1, 1};
};
struct PreviewCamera final
{
    QString Id;
    QString Name;
    QString FollowId;
    gameplay::camera::CameraSample Sample;
    gameplay::camera::CameraTrace Trace;
};
/// Viewpoint an imported scene opens at: the World level's `camera`, which an import sets from
/// the source scene's camera (or frames on its bounds) and "Set start view" replaces.
struct PreviewStartView final
{
    foundation::math::Vector3d EyeWorld{};
    /// Point looked at; its distance from the eye becomes the editor camera's focus distance.
    foundation::math::Vector3d TargetWorld{};
    foundation::math::Vector3 UpWorld{0, 1, 0};
    /// Vertical field of view in radians, in (0, pi).
    foundation::float32 VerticalFov = 1;
};
/// Light SourceId of an authored World light entity, and the object SourceId of an authored
/// renderable, as World composition writes them: a nonzero digest of the entity key.
[[nodiscard]] foundation::uint64 ComputeEntitySourceId(const QString& entityKey);
/// Parse a level `camera` object ({eye, target, up, fov}, world units and radians). Failure,
/// including a degenerate view or a field of view outside (0, pi), preserves outView.
[[nodiscard]] bool TryParseStartView(const QJsonObject& camera, PreviewStartView& outView);
/// Level `camera` object describing view; the inverse of TryParseStartView.
[[nodiscard]] QJsonObject MakeStartViewJson(const PreviewStartView& view);
struct PreviewViewOptions final
{
    foundation::int32 Camera = -1;
    foundation::math::Vector2 OrbitDegrees{25, 20};
    foundation::float32 Distance = 3;
    foundation::uint32 Width = 0, Height = 0;
    bool Reference = false;
    const foundation::math::Vector3* OrbitCenter = nullptr;
    foundation::float32 OrbitRadius = 0;
    const gameplay::camera::CameraSample* EditorCameraOrNull = nullptr;
    /// Produce DepthConvention::Conventional (near 0, far 1, Less) instead of the viewport's
    /// reverse-Z. An infinite perspective lens is replaced by a finite one whose far plane
    /// encloses the World's geometry from the camera, so clip z can be rewritten as w - z exactly.
    bool ShouldUseConventionalDepth = false;
    /// Optional precomputed TryGetBounds({}) result for this scene, borrowed for the call.
    /// Conventional depth then bounds the far plane without transforming every vertex again.
    /// Null computes the bounds; a caller must not pass bounds of a different scene or transforms.
    const foundation::math::Aabb3* BoundsWorldOrNull = nullptr;
};
struct RenderScene final
{
    // Immutable decoded snapshot shared by every retained copy of the scene
    // (workspace, load job, preview residency, GPU generation, static-lighting
    // bake). The last holder to release it, on any thread, destroys it.
    foundation::SharedPtr<const resources::SceneDefinition> Definition;
    QString LogicalId;
    bool Lit = false;
    graphics::renderer::LightSelection AuthoredLights{};
    /// Enabled authored analytic sources that opted into the shared always-on bake.
    QVector<foundation::uint64> AlwaysOnLightIds;
    graphics::renderer::AtmosphereDescription Atmosphere{};
    bool IsAtmosphereSunSwitchedOff = false;
    foundation::uint64 AtmosphereMoonSourceId = 0;
    bool IsAtmosphereMoonSwitchedOff = false;
    QString AtmosphereStatus;
    graphics::renderer::GiProbeGrid ProbeGrid{};
    QVector<foundation::uint64> GiProbeReceiverIds;
    /// Enabled World panel luminaires (the preview descriptor's `luminaire` binding), admitted by
    /// NormalizePanelLuminaires; each SourceId is its entity key's source identity. They light only scene shaders
    /// that read them, such as the slice-sampling-luminaire template, and Merge keeps the authored snapshot's.
    graphics::renderer::PanelLuminaireSelection Luminaires{};
    /// Project-owned luminaire property binding; empty when the preview descriptor declares none.
    QJsonObject LuminaireBinding;
    // Worker decode; no source importer, engine code execution or device access.
    [[nodiscard]] static bool LoadCooked(const QByteArray& bytes,
                                         const QJsonObject& level,
                                         RenderScene& output,
                                         QString& error,
                                         const QJsonObject& revisions);
    // Combine schema-bound entities and imported geometry before renderer admission.
    [[nodiscard]] static bool Merge(const RenderScene& authored,
                                    const QByteArray& cooked,
                                    const QJsonObject& level,
                                    RenderScene& output,
                                    QString& error,
                                    const QJsonObject& revisions);
    /// The enabled analytic lights of a cooked definition, normalized as LoadCooked presents them.
    /// Failure (more lights than the viewport admits) preserves outLights.
    [[nodiscard]] static bool LoadCookedLights(const resources::SceneDefinition& scene,
                                               graphics::renderer::LightSelection& outLights,
                                               QString& outError);
    QVector<PreviewMesh> Meshes;
    QVector<PreviewTexture> Textures;
    QVector<PreviewObject> Objects;
    QVector<PreviewSurface> Surfaces;
    QVector<PreviewCamera> Cameras;
    foundation::math::Vector3 Center{};
    foundation::float32 Radius = 1;
    QByteArray Identity;
    QByteArray PacketIdentity;
    QByteArray AssetsIdentity;
    /// Identity before ApplyLightSwitches disabled any source; empty while nothing is disabled.
    QByteArray UnswitchedIdentity;
    /// Digest of the disabled light sources this scene presents; empty while nothing is disabled.
    /// Lighting keys hash it because the switched emission is not part of the definition.
    QByteArray LightSwitchIdentity;
    QJsonObject RenderableBinding;
    /// Project-owned position/Euler-degree/scale property binding for viewport manipulation.
    QJsonObject TransformBinding;
    /// Project-owned light binding ({component, properties}); empty when the World binds no lights.
    QJsonObject LightBinding;
    /// Camera the viewport looks through until the user picks another; -1 is the editor camera.
    foundation::int32 DefaultCamera = -1;
    /// Level start view of an imported scene; absent for Worlds without a scene level.
    /// Not part of Identity, so changing only the start view never reloads GPU resources.
    PreviewStartView StartView;
    bool HasStartView = false;
    /// Choose the extraction source for a World in a project root; reads only file metadata.
    [[nodiscard]] static RenderSceneSource GetSource(const QString& root, const WorldDocument& world);
    /// Load authored objects. Disable lighting composition when passing this snapshot to Merge,
    /// which must consume the original mesh data rather than an already cooked scene.
    [[nodiscard]] static bool Load(const QString& root,
                                   const WorldDocument& world,
                                   RenderScene& outScene,
                                   QString& outError,
                                   bool shouldComposeLighting = true);
    /// Copy the atmosphere with preview light switches applied.
    [[nodiscard]] graphics::renderer::AtmosphereDescription GetAtmosphereForView() const noexcept;
    /// True when a loaded World has neither drawable objects nor an Atmosphere request.
    [[nodiscard]] bool ShouldUseEmptyWorldView() const noexcept;
    /// Bounds of the extracted visible objects, or one selected stable ID; failure preserves output.
    [[nodiscard]] bool TryGetBounds(const QString& id, foundation::math::Aabb3& outBoundsWorld) const noexcept;
    // camera=-1 selects the independent editor orbit. All failures preserve output.
    // Cooked scenes (Definition set) read their geometry from the definition for bounds.
    [[nodiscard]] bool
    View(const PreviewViewOptions& options, graphics::renderer::ViewDescription& outView, QString& outError) const;
};
/// The stable nonzero source identity RenderScene::Load gives the light, luminaire or object of World entity key
/// entityKey (WorldDocument::GetEntityKey).
[[nodiscard]] foundation::uint64 ComputeRenderSceneSourceId(const QString& entityKey);
/// Mesh-local positions of scene mesh meshIndex, from the cooked Definition when the scene has one and from the
/// authored PreviewMesh otherwise. meshIndex must index scene.Meshes; the view borrows the scene's storage.
[[nodiscard]] foundation::BorrowedView<const foundation::math::Vector3>
GetRenderSceneMeshPositions(const RenderScene& scene, foundation::int32 meshIndex) noexcept;
/// Triangle-list indices of scene mesh meshIndex, from the same storage as GetRenderSceneMeshPositions.
[[nodiscard]] foundation::BorrowedView<const foundation::uint32>
GetRenderSceneMeshIndices(const RenderScene& scene, foundation::int32 meshIndex) noexcept;
/// Intersect a mesh-space ray with one triangle in double precision, from either side.
/// @return True with outDistance, the nonnegative ray parameter of the hit; false preserves outDistance.
[[nodiscard]] bool IntersectRenderSceneTriangle(const foundation::math::Ray3& rayMesh,
                                                foundation::math::Vector3 positionMeshA,
                                                foundation::math::Vector3 positionMeshB,
                                                foundation::math::Vector3 positionMeshC,
                                                foundation::float64& outDistance) noexcept;
/// Nearest hit of a world ray with the scene's visible objects, brute force over every triangle; for one-off editor
/// queries, not per frame. outDistanceWorld is in units of the ray direction's length.
/// @return False when nothing is hit; outDistanceWorld is then unchanged.
[[nodiscard]] bool TryTraceRenderSceneRay(const RenderScene& scene,
                                          const foundation::math::Ray3& rayWorld,
                                          foundation::float64& outDistanceWorld) noexcept;
} // namespace ludus::editor
