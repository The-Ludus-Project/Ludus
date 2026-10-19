#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/runtime/resources/scene.h>

#include "internal/world_document.h"

#include <QImage>
#include <QJsonObject>
#include <QVector>

namespace ludus::editor
{
/// Where a World's render extraction comes from; one World has at most one source.
enum class RenderSceneSource : foundation::uint8
{
    /// Nothing renders the World yet: no project bindings and no game adapter.
    None,
    /// The project's authoring/render.preview.json binds World components to renderables.
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
};
struct PreviewSurface final
{
    foundation::int32 Texture = 0;
    foundation::math::Vector4 Tint{1, 1, 1, 1};
    /// Straight linear RGBA base color for consumers that sample no textures, such as the
    /// render-graph RendererStatic preview. Authored surfaces use Tint. Cooked surfaces
    /// compute it once at decode: Tint times the linear mean of the base-color texture's mip-zero
    /// RGB, with alpha one because the cooked viewport transport draws LUSCN materials Opaque.
    /// Untextured cooked surfaces keep their black Tint. The lit and textured paths ignore it.
    foundation::math::Vector4 FlatTint{1, 1, 1, 1};
    bool Repeat = false;
    foundation::float32 Metallic = 0;
    foundation::float32 Roughness = .5F;
    foundation::math::Vector3 Emission{};
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
    QJsonObject RenderableBinding;
    /// Camera the viewport looks through until the user picks another; -1 is the editor camera.
    foundation::int32 DefaultCamera = -1;
    /// Choose the extraction source for a World in a project root; reads only file metadata.
    [[nodiscard]] static RenderSceneSource GetSource(const QString& root, const WorldDocument& world);
    [[nodiscard]] static bool
    Load(const QString& root, const WorldDocument& world, RenderScene& output, QString& error);
    /// Bounds of the extracted visible objects, or one selected stable ID; failure preserves output.
    [[nodiscard]] bool TryGetBounds(const QString& id, foundation::math::Aabb3& outBoundsWorld) const noexcept;
    // camera=-1 selects the independent editor orbit. All failures preserve output.
    // Cooked scenes (Definition set) read their geometry from the definition for bounds.
    [[nodiscard]] bool
    View(const PreviewViewOptions& options, graphics::renderer::ViewDescription& outView, QString& outError) const;
};
} // namespace ludus::editor
