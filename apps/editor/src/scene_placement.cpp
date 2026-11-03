#include "internal/scene_placement.h"

#include "internal/light_switches.h"

#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/runtime/resources/scene.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <bit>
#include <span>
#include <string_view>

namespace ludus::editor
{
using namespace foundation;
namespace
{
namespace format = content::scene_format;
/// Spatial axes: basis columns, and components of each column and of the translation.
constexpr usize AXIS_COUNT = 3;
constexpr usize TRANSLATION_OFFSET = AXIS_COUNT * AXIS_COUNT;

/// The level without object transforms: equal for two levels whose cooks differ only in placement.
QJsonObject RemoveObjectTransforms(const QJsonObject& level)
{
    QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    for (qsizetype i = 0; i < objects.size(); ++i)
    {
        QJsonObject object = objects[i].toObject();
        object.remove(QStringLiteral("transform"));
        objects[i] = object;
    }
    QJsonObject result = level;
    result.insert(QStringLiteral("objects"), objects);
    return result;
}

/// One object record of ComposeLevelTransforms: the column-major basis, then the translation.
math::Affine3 ReadMeshToWorld(const float32* values) noexcept
{
    math::Affine3 meshToWorld;
    for (usize column = 0; column < AXIS_COUNT; ++column)
    {
        const float32* basis = values + column * AXIS_COUNT;
        meshToWorld.Columns[column] = {basis[0], basis[1], basis[2]};
    }
    meshToWorld.Translation = {values[TRANSLATION_OFFSET],
                               values[TRANSLATION_OFFSET + 1],
                               values[TRANSLATION_OFFSET + 2]};
    return meshToWorld;
}

[[nodiscard]] bool AreBitwiseEqual(math::Vector3 a, math::Vector3 b) noexcept
{
    return std::bit_cast<uint32>(a.X) == std::bit_cast<uint32>(b.X) &&
           std::bit_cast<uint32>(a.Y) == std::bit_cast<uint32>(b.Y) &&
           std::bit_cast<uint32>(a.Z) == std::bit_cast<uint32>(b.Z);
}

[[nodiscard]] bool AreBitwiseEqual(const math::Affine3& a, const math::Affine3& b) noexcept
{
    return AreBitwiseEqual(a.Columns[0], b.Columns[0]) == true && AreBitwiseEqual(a.Columns[1], b.Columns[1]) == true &&
           AreBitwiseEqual(a.Columns[2], b.Columns[2]) == true && AreBitwiseEqual(a.Translation, b.Translation) == true;
}

/// Hash a previous identity with every new object transform, so equal placements give equal identities.
QByteArray DeriveIdentity(const QByteArray& previous, const Array<float32>& transforms)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("ludus-scene-placement-v1"));
    hash.addData(previous);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(transforms.GetData()),
                                static_cast<qsizetype>(transforms.GetSize() * sizeof(float32))));
    return hash.result();
}
} // namespace

PlacedSceneStatus DerivePlacedScene(const RenderScene& presented,
                                    const QJsonObject& presentedLevel,
                                    const QJsonObject& level,
                                    RenderScene& outScene,
                                    QString& outError)
{
    const QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    const usize objectCount = static_cast<usize>(objects.size());
    if (presented.Definition == nullptr || presented.Lit == false ||
        presented.Definition->Objects.GetSize() != static_cast<usize>(presented.Objects.size()) ||
        static_cast<usize>(presented.Objects.size()) < objectCount ||
        RemoveObjectTransforms(level) != RemoveObjectTransforms(presentedLevel))
    {
        return PlacedSceneStatus::RequiresAssembly;
    }
    // The cook's own composition, from the same compact bytes LevelWorkspace hands it.
    const QByteArray bytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    Array<float32> transforms;
    content::scene_cook::AssetError cookError;
    if (content::scene_cook::ComposeLevelTransforms(
            {reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())},
            transforms,
            cookError) == false)
    {
        const std::string_view message = cookError.Message.View();
        outError = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        return PlacedSceneStatus::RequiresAssembly;
    }
    LUDUS_ASSERT(transforms.GetSize() == objectCount * format::OBJECT_TRANSFORM_COMPONENTS);
    QSet<QString> moved;
    for (usize i = 0; i < objectCount; ++i)
    {
        const PreviewObject& object = presented.Objects[static_cast<qsizetype>(i)];
        if (object.Id != objects[static_cast<qsizetype>(i)].toObject().value(QStringLiteral("id")).toString())
        {
            return PlacedSceneStatus::RequiresAssembly;
        }
        const math::Affine3 meshToWorld =
            ReadMeshToWorld(transforms.GetData() + i * format::OBJECT_TRANSFORM_COMPONENTS);
        if (AreBitwiseEqual(meshToWorld, presented.Definition->Objects[i].Transform) == false)
        {
            // The runtime decoder admits only invertible object transforms; so does publication.
            math::Affine3 worldToMesh;
            if (math::TryInverse(meshToWorld, worldToMesh) != math::MathStatus::Success)
            {
                outError = QStringLiteral("%1 has a singular placement.").arg(object.Name);
                return PlacedSceneStatus::RequiresAssembly;
            }
            moved.insert(object.Id);
        }
    }
    if (moved.isEmpty() == true)
    {
        return PlacedSceneStatus::Unchanged;
    }
    // Composed World objects and cameras read entity transforms themselves; a moved
    // instance they name needs the full composition.
    for (qsizetype i = static_cast<qsizetype>(objectCount); i < presented.Objects.size(); ++i)
    {
        if (moved.contains(presented.Objects[i].Id) == true)
        {
            return PlacedSceneStatus::RequiresAssembly;
        }
    }
    for (const PreviewCamera& camera : presented.Cameras)
    {
        if (moved.contains(camera.Id) == true || moved.contains(camera.FollowId) == true)
        {
            return PlacedSceneStatus::RequiresAssembly;
        }
    }
    SharedPtr<resources::SceneDefinition> definition;
    if (TryMakeShared<resources::SceneDefinition>(GetSystemAllocationDomain(), definition) != OwnershipStatus::Ok ||
        presented.Definition->TryCopyTo(*definition) != resources::Status::Ok)
    {
        outError = QStringLiteral("Placed scene allocation failed.");
        return PlacedSceneStatus::RequiresAssembly;
    }
    RenderScene candidate = presented;
    for (usize i = 0; i < objectCount; ++i)
    {
        const math::Affine3 meshToWorld =
            ReadMeshToWorld(transforms.GetData() + i * format::OBJECT_TRANSFORM_COMPONENTS);
        definition->Objects[i].Transform = meshToWorld;
        candidate.Objects[static_cast<qsizetype>(i)].Transform = meshToWorld;
    }
    // Preview textures borrow definition pixels, so they must borrow the copy's.
    for (qsizetype i = 0; i < candidate.Textures.size(); ++i)
    {
        const resources::SceneTexture& source = definition->Textures[static_cast<usize>(i)];
        candidate.Textures[i].Pixels = QImage(source.Pixels.GetData(),
                                              static_cast<int32>(source.Width),
                                              static_cast<int32>(source.Height),
                                              QImage::Format_RGBA8888);
        if (candidate.Textures[i].Pixels.isNull() == true)
        {
            outError = QStringLiteral("Placed scene texture allocation failed.");
            return PlacedSceneStatus::RequiresAssembly;
        }
    }
    candidate.Definition = Move(definition);
    candidate.PacketIdentity = DeriveIdentity(presented.PacketIdentity, transforms);
    if (presented.UnswitchedIdentity.isEmpty() == true)
    {
        candidate.Identity = DeriveIdentity(presented.Identity, transforms);
    }
    else
    {
        // Keep the switched-off light sources: the moved content gets a new unswitched identity,
        // so a later ApplyLightSwitches composes from it rather than from the old placement.
        candidate.UnswitchedIdentity = DeriveIdentity(presented.UnswitchedIdentity, transforms);
        candidate.Identity = ComposeLightSwitchedIdentity(candidate.UnswitchedIdentity, presented.LightSwitchIdentity);
    }
    outScene = Move(candidate);
    outError.clear();
    return PlacedSceneStatus::Derived;
}

bool IsSamePlacedScene(const RenderScene& derived, const RenderScene& cooked) noexcept
{
    if (derived.Definition == nullptr || cooked.Definition == nullptr || derived.Lit != cooked.Lit ||
        derived.LogicalId != cooked.LogicalId || derived.AssetsIdentity != cooked.AssetsIdentity ||
        derived.Meshes.size() != cooked.Meshes.size() || derived.Textures.size() != cooked.Textures.size() ||
        derived.Surfaces.size() != cooked.Surfaces.size() || derived.Objects.size() != cooked.Objects.size() ||
        derived.Cameras.size() != cooked.Cameras.size() || derived.DefaultCamera != cooked.DefaultCamera ||
        derived.AuthoredLights.Count != cooked.AuthoredLights.Count ||
        derived.TransformBinding != cooked.TransformBinding || derived.RenderableBinding != cooked.RenderableBinding)
    {
        return false;
    }
    for (qsizetype i = 0; i < derived.Objects.size(); ++i)
    {
        const PreviewObject& left = derived.Objects[i];
        const PreviewObject& right = cooked.Objects[i];
        if (left.Id != right.Id || left.Name != right.Name || left.Mesh != right.Mesh ||
            left.Surface != right.Surface || left.Texture != right.Texture ||
            AreBitwiseEqual(left.Transform, right.Transform) == false)
        {
            return false;
        }
    }
    for (qsizetype i = 0; i < derived.Cameras.size(); ++i)
    {
        if (derived.Cameras[i].Id != cooked.Cameras[i].Id ||
            derived.Cameras[i].FollowId != cooked.Cameras[i].FollowId ||
            (derived.Cameras[i].Sample.Pose.Position == cooked.Cameras[i].Sample.Pose.Position) == false)
        {
            return false;
        }
    }
    const resources::SceneDefinition& left = *derived.Definition;
    const resources::SceneDefinition& right = *cooked.Definition;
    if (left.Meshes.GetSize() != right.Meshes.GetSize() || left.Materials.GetSize() != right.Materials.GetSize() ||
        left.Textures.GetSize() != right.Textures.GetSize() || left.Objects.GetSize() != right.Objects.GetSize() ||
        left.Lights.GetSize() != right.Lights.GetSize() || left.Environment.Width != right.Environment.Width ||
        left.Environment.Height != right.Environment.Height ||
        AreBitwiseEqual(left.Environment.Rgb, right.Environment.Rgb) == false ||
        AreBitwiseEqual(left.Eye, right.Eye) == false || AreBitwiseEqual(left.Target, right.Target) == false ||
        AreBitwiseEqual(left.Up, right.Up) == false ||
        std::bit_cast<uint32>(left.VerticalFov) != std::bit_cast<uint32>(right.VerticalFov))
    {
        return false;
    }
    for (usize i = 0; i < left.Objects.GetSize(); ++i)
    {
        if (left.Objects[i].Mesh != right.Objects[i].Mesh || left.Objects[i].Material != right.Objects[i].Material ||
            left.Objects[i].SourceId != right.Objects[i].SourceId ||
            AreBitwiseEqual(left.Objects[i].Transform, right.Objects[i].Transform) == false)
        {
            return false;
        }
    }
    for (usize i = 0; i < left.Lights.GetSize(); ++i)
    {
        if (left.Lights[i].SourceId != right.Lights[i].SourceId ||
            AreBitwiseEqual(left.Lights[i].PositionWorld, right.Lights[i].PositionWorld) == false ||
            AreBitwiseEqual(left.Lights[i].IntensityRgb, right.Lights[i].IntensityRgb) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::editor
