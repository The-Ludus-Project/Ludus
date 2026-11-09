#include "internal/world_aperture.h"

#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/content/scene_source/scene_source.h>

#include <QCryptographicHash>

#include <string_view>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;
namespace source = content::scene_source;

/// Prefix of a generated aperture entity ID; a hash of its level, source file and object follows.
constexpr std::string_view IMPORTED_APERTURE_ID_PREFIX = "aperture-";
/// Hexadecimal digits of the SHA-256 kept in an imported aperture's entity ID (128 bits).
constexpr qsizetype IMPORTED_APERTURE_ID_DIGITS = 32;
/// Triangles of one aperture quad, fanned from its first corner.
constexpr uint32 TRIANGLES_PER_QUAD = 2;
/// Corners of one triangle.
constexpr uint32 CORNERS_PER_TRIANGLE = 3;
/// Quad corners of the two fan triangles, as a scene source import cooks a quad.
constexpr StaticArray<uint32, TRIANGLES_PER_QUAD * CORNERS_PER_TRIANGLE> QUAD_FAN_CORNERS{{0, 1, 2, 0, 2, 3}};
/// Below this cosine of the frame's tilt the Euler Y rotation is not at +-90 degrees.
constexpr float64 EULER_GIMBAL_COSINE = 1e-12;

[[nodiscard]] source::ApertureShape ReadShape(const QJsonObject& values)
{
    return
    {
        .ScreenWidth = values.value(QStringLiteral("screenWidth")).toDouble(),
        .ScreenHeight = values.value(QStringLiteral("screenHeight")).toDouble(),
        .SlitLength = values.value(QStringLiteral("slitLength")).toDouble(),
        .Opening = values.value(QStringLiteral("opening")).toDouble(),
    };
}

/// Fixed-axis Euler angles in degrees (X, then Y, then Z: R = Rz * Ry * Rx) of an orthonormal frame,
/// the convention of the default World's transform component.
[[nodiscard]] math::Vector3d
ComputeEulerDegrees(const math::Vector3d& acrossWorld, const math::Vector3d& upWorld, const math::Vector3d& normalWorld)
{
    // Columns of R are the frame's axes, so R[row][column] is component `row` of axis `column`.
    const float64 cosineY = math::Sqrt(acrossWorld.X * acrossWorld.X + acrossWorld.Y * acrossWorld.Y);
    float64 x = 0;
    float64 z = 0;
    const float64 y = math::Atan2(-acrossWorld.Z, cosineY);
    if (cosineY > EULER_GIMBAL_COSINE)
    {
        x = math::Atan2(upWorld.Z, normalWorld.Z);
        z = math::Atan2(acrossWorld.Y, acrossWorld.X);
    }
    else
    {
        // Gimbal lock: only x - z (or x + z) is defined; keep z at zero.
        x = math::Atan2(-normalWorld.Y, upWorld.Y);
    }
    return {math::RadiansToDegrees(x), math::RadiansToDegrees(y), math::RadiansToDegrees(z)};
}

[[nodiscard]] QJsonObject MakeDefaultValues(const WorldDocument& world, const QString& component)
{
    QJsonObject values;
    for (const QJsonValue property : world.Component(component).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject definition = property.toObject();
        values.insert(definition.value(QStringLiteral("id")).toString(), definition.value(QStringLiteral("default")));
    }
    return values;
}

[[nodiscard]] QString ToQString(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace

QString GetApertureComponentId()
{
    return QStringLiteral("aperture");
}

bool ValidateApertureParameters(const QJsonObject& values, QString& outError)
{
    const source::ApertureShape shape = ReadShape(values);
    if (shape.Opening >= shape.ScreenWidth)
    {
        outError = QStringLiteral("Aperture Opening (%1 m) must be less than Screen width (%2 m). Narrow the opening "
                                  "or widen the screen.")
                       .arg(shape.Opening)
                       .arg(shape.ScreenWidth);
        return false;
    }
    if (shape.SlitLength > shape.ScreenHeight)
    {
        outError = QStringLiteral("Aperture Slit length (%1 m) must be at most Screen height (%2 m). Shorten the slit "
                                  "or make the screen taller.")
                       .arg(shape.SlitLength)
                       .arg(shape.ScreenHeight);
        return false;
    }
    if (source::IsApertureShapeValid(shape) == false)
    {
        outError = QStringLiteral("Aperture sizes must be finite, with positive screen and slit sizes.");
        return false;
    }
    return true;
}

bool BuildApertureMesh(const QJsonObject& values, PreviewMesh& outMesh, QString& outError)
{
    if (ValidateApertureParameters(values, outError) == false)
    {
        return false;
    }
    const source::ApertureShape shape = ReadShape(values);
    source::ApertureQuads quads;
    if (source::BuildApertureQuads(shape, quads) == false)
    {
        outError = QStringLiteral("Aperture sizes must be finite, with positive screen and slit sizes.");
        return false;
    }
    PreviewMesh mesh;
    mesh.Id = outMesh.Id;
    const qsizetype cornerCount = static_cast<qsizetype>(quads.Count * source::APERTURE_QUAD_CORNERS);
    mesh.Positions.reserve(cornerCount);
    mesh.Normals.reserve(cornerCount);
    mesh.Uvs.reserve(cornerCount);
    mesh.Indices.reserve(static_cast<qsizetype>(quads.Count * TRIANGLES_PER_QUAD * CORNERS_PER_TRIANGLE));
    for (usize q = 0; q < quads.Count; ++q)
    {
        const uint32 first = static_cast<uint32>(mesh.Positions.size());
        for (const math::Vector3d& corner : quads.Quads[q])
        {
            mesh.Positions.append({static_cast<float32>(corner.X), static_cast<float32>(corner.Y), 0});
            mesh.Normals.append({0, 0, 1});
            // U runs across the screen and V down it, in the top-down texture convention.
            mesh.Uvs.append({static_cast<float32>(corner.X / shape.ScreenWidth + 0.5),
                             static_cast<float32>(0.5 - corner.Y / shape.ScreenHeight)});
        }
        for (const uint32 corner : QUAD_FAN_CORNERS)
        {
            mesh.Indices.append(first + corner);
        }
    }
    outMesh = Move(mesh);
    return true;
}

math::Vector4 GetApertureReflectance(const QJsonObject& values)
{
    return {static_cast<float32>(values.value(QStringLiteral("r")).toDouble()),
            static_cast<float32>(values.value(QStringLiteral("g")).toDouble()),
            static_cast<float32>(values.value(QStringLiteral("b")).toDouble()),
            1};
}

bool MakeImportedApertures(const QByteArray& sourceBytes,
                           const WorldDocument& world,
                           const QString& levelId,
                           const QString& entryName,
                           ImportedApertures& outApertures,
                           QString& outError)
{
    if (world.HasBuiltInAperture() == false)
    {
        outError = QStringLiteral("Open a default World to import apertures as editable entities.");
        return false;
    }
    source::SceneSource scene;
    source::SceneSourceError error;
    if (source::ParseSceneSource(std::string_view(sourceBytes.constData(), static_cast<usize>(sourceBytes.size())),
                                 scene,
                                 error) != source::SceneSourceStatus::Success)
    {
        outError = QStringLiteral("%1: %2").arg(entryName, ToQString(error.Message.View()));
        return false;
    }
    const QString component = GetApertureComponentId();
    ImportedApertures apertures;
    for (const source::SourceObject& object : scene.Objects)
    {
        if (object.Aperture.IsPresent == false)
        {
            continue;
        }
        const source::SourceAperture& aperture = object.Aperture;
        const QString name = ToQString(object.Name.GetView());
        const source::SourceMaterial& material = scene.Materials[object.Material];
        const QString materialName = ToQString(material.Name.GetView());
        QJsonObject transform = MakeDefaultValues(world, QStringLiteral("transform"));
        transform.insert(QStringLiteral("x"), aperture.CenterWorld.X);
        transform.insert(QStringLiteral("y"), aperture.CenterWorld.Y);
        transform.insert(QStringLiteral("z"), aperture.CenterWorld.Z);
        const math::Vector3d acrossWorld = math::Cross(aperture.UpWorld, aperture.NormalWorld);
        const math::Vector3d rotation = ComputeEulerDegrees(acrossWorld, aperture.UpWorld, aperture.NormalWorld);
        transform.insert(QStringLiteral("rx"), rotation.X);
        transform.insert(QStringLiteral("ry"), rotation.Y);
        transform.insert(QStringLiteral("rz"), rotation.Z);
        QJsonObject values = MakeDefaultValues(world, component);
        values.insert(QStringLiteral("opening"), aperture.Shape.Opening);
        values.insert(QStringLiteral("slitLength"), aperture.Shape.SlitLength);
        values.insert(QStringLiteral("screenWidth"), aperture.Shape.ScreenWidth);
        values.insert(QStringLiteral("screenHeight"), aperture.Shape.ScreenHeight);
        values.insert(QStringLiteral("r"), material.BaseColor.X);
        values.insert(QStringLiteral("g"), material.BaseColor.Y);
        values.insert(QStringLiteral("b"), material.BaseColor.Z);
        if (material.Model != source::SourceMaterialModel::Diffuse)
        {
            apertures.Warnings.append(QStringLiteral("Aperture '%1' reflects as Lambertian with the base colour of "
                                                     "material '%2'; its other reflection terms are not kept.")
                                          .arg(name, materialName));
        }
        if (material.EmittedRadiance != math::Vector3d{})
        {
            apertures.Warnings.append(QStringLiteral("Aperture '%1' does not emit: material '%2' emission is not kept.")
                                          .arg(name, materialName));
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(levelId.toUtf8());
        hash.addData(QByteArrayLiteral("\n"));
        hash.addData(entryName.toUtf8());
        hash.addData(QByteArrayLiteral("\n"));
        hash.addData(QByteArray(object.Name.GetView().data(), static_cast<qsizetype>(object.Name.GetView().size())));
        const QString id = ToQString(IMPORTED_APERTURE_ID_PREFIX) +
                           QString::fromLatin1(hash.result().toHex().left(IMPORTED_APERTURE_ID_DIGITS));
        apertures.Entities.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("components"), QJsonObject{{QStringLiteral("transform"), transform}, {component, values}}},
        });
    }
    outApertures = Move(apertures);
    return true;
}
} // namespace ludus::editor
