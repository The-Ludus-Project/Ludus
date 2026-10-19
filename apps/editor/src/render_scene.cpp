#include "internal/render_scene.h"
#include "internal/qt_numeric_stream.h"
#include "internal/render_scene_limits.h"

#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <string_view>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>

#include <QSet>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;
namespace format = ludus::content::scene_format;
namespace camera = gameplay::camera;
namespace rr = graphics::renderer;
bool Fail(QString& error, const QString& text)
{
    error = text;
    return false;
}
/// Distinct codes of one 8-bit texture channel.
constexpr usize CHANNEL_CODES = 256;
/// Largest 8-bit channel code, which encodes 1.
constexpr float32 MAX_CHANNEL_CODE = 255.0F;
/// Color channels averaged by ComputeLinearMeanRgb; alpha is the fourth texel byte.
constexpr usize COLOR_CHANNELS = 3;
/// Flat base color of a decoded cooked texture for consumers that sample no textures: the
/// unweighted mean of its mip-zero RGB in linear space. Each sRGB byte is decoded before
/// averaging, never averaged as a code. Alpha is ignored, because the cooked viewport transport
/// draws LUSCN materials Opaque and samples no alpha. Counting codes per channel makes the result
/// independent of texel order. Decoding already bounded the texture by the SceneFormat texel and
/// byte caps, so the work is linear in at most MAX_TOTAL_TEXTURE_BYTES per scene.
math::Vector3 ComputeLinearMeanRgb(const resources::SceneTexture& texture) noexcept
{
    const usize texelCount = texture.Pixels.GetSize() / format::TEXEL_BYTES;
    if (texelCount == 0)
    {
        return {};
    }
    StaticArray<uint64, CHANNEL_CODES * COLOR_CHANNELS> counts{};
    const uint8* bytes = texture.Pixels.GetData();
    for (usize texel = 0; texel < texelCount; ++texel)
    {
        for (usize channel = 0; channel < COLOR_CHANNELS; ++channel)
        {
            ++counts[channel * CHANNEL_CODES + bytes[texel * format::TEXEL_BYTES + channel]];
        }
    }
    StaticArray<float64, COLOR_CHANNELS> sums{};
    for (usize code = 0; code < CHANNEL_CODES; ++code)
    {
        const float64 linear = static_cast<float64>(math::DecodeSrgb(static_cast<float32>(code) / MAX_CHANNEL_CODE));
        for (usize channel = 0; channel < COLOR_CHANNELS; ++channel)
        {
            sums[channel] += linear * static_cast<float64>(counts[channel * CHANNEL_CODES + code]);
        }
    }
    const float64 count = static_cast<float64>(texelCount);
    return {static_cast<float32>(sums[0] / count),
            static_cast<float32>(sums[1] / count),
            static_cast<float32>(sums[2] / count)};
}
/// Corners of an axis-aligned box.
constexpr usize AABB_CORNER_COUNT = 8;
/// Unitless scale on the farthest scene-bound corner distance, so geometry on the bound is not
/// clipped by rounding at a finite far plane.
constexpr float32 FINITE_FAR_MARGIN = 1.01F;
/// Minimum far-to-near ratio of a finite preview projection; a scene entirely inside the near
/// plane still yields a valid (empty) frustum.
constexpr float32 MIN_FAR_TO_NEAR = 2.0F;
/// Mesh-local positions of scene mesh meshIndex. A cooked scene (Definition set) keeps its
/// geometry only in the decoded definition, whose mesh table parallels RenderScene::Meshes.
/// @pre meshIndex indexes scene.Meshes and, when set, the definition's mesh table.
BorrowedView<const math::Vector3> GetMeshPositions(const RenderScene& scene, int32 meshIndex) noexcept
{
    if (scene.Definition != nullptr)
    {
        const resources::SceneMesh& mesh = scene.Definition->Meshes[static_cast<usize>(meshIndex)];
        return {mesh.Positions.GetData(), mesh.Positions.GetSize()};
    }
    const PreviewMesh& mesh = scene.Meshes[meshIndex];
    return {mesh.Positions.constData(), static_cast<usize>(mesh.Positions.size())};
}
/// Rewrite a finite reverse-Z view (near 1, far 0, Greater) for the conventional convention
/// (near 0, far 1, Less) by replacing clip z with w - z. Exact only for a finite projection: an
/// infinite one would map its absent far plane to the degenerate depth 1 at infinity.
void ConvertToConventionalDepth(rr::ViewDescription& inoutView) noexcept
{
    for (math::Vector4& column : inoutView.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    inoutView.Depth = rr::DepthConvention::Conventional;
    inoutView.InfiniteFar = false;
}
uint64 ComputeSourceId(const QString& id)
{
    const QByteArray hash = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
    uint64 value = 0;
    for (usize i = 0; i < sizeof(value); ++i)
    {
        value |= static_cast<uint64>(static_cast<uint8>(hash[static_cast<int32>(i)])) << (i * 8);
    }
    return value == 0 ? 1 : value;
}
// LUSCN v4 mesh: indexed VertexRecords; consumers derive the per-triangle bake charts.
// Meshes without normals are expanded per corner so each triangle keeps its face normal.
bool WriteSceneMesh(QDataStream& inoutStream,
                    foundation::BorrowedView<const math::Vector3> positions,
                    foundation::BorrowedView<const math::Vector3> normals,
                    foundation::BorrowedView<const math::Vector2> texcoords,
                    foundation::BorrowedView<const uint32> indices,
                    usize& inoutVertexCount,
                    usize& inoutIndexCount,
                    QString& outError)
{
    format::BakeChartLayout layout;
    const bool isExpanded = normals.IsEmpty() == true;
    const usize vertexCount = isExpanded == true ? indices.GetSize() : positions.GetSize();
    if (positions.IsValid() == false || normals.IsValid() == false || texcoords.IsValid() == false ||
        indices.IsValid() == false || positions.IsEmpty() == true || indices.IsEmpty() == true ||
        indices.GetSize() % format::TRIANGLE_CORNERS != 0 || texcoords.GetSize() != positions.GetSize() ||
        (isExpanded == false && normals.GetSize() != positions.GetSize()) ||
        vertexCount > format::MAX_VERTICES - inoutVertexCount || indices.GetSize() > format::MAX_INDICES ||
        indices.GetSize() > format::MAX_TOTAL_INDICES - inoutIndexCount ||
        format::TryMakeBakeChartLayout(static_cast<uint32>(indices.GetSize() / 3), layout) == false)
    {
        return Fail(outError, QStringLiteral("World geometry exceeds the scene vertex or index capacity."));
    }
    for (usize i = 0; i < indices.GetSize(); ++i)
    {
        if (indices[i] >= positions.GetSize())
        {
            return Fail(outError, QStringLiteral("Invalid World mesh index."));
        }
    }
    inoutVertexCount += vertexCount;
    inoutIndexCount += indices.GetSize();
    inoutStream << static_cast<uint32>(vertexCount) << static_cast<uint32>(indices.GetSize());
    if (isExpanded == false)
    {
        for (usize vertex = 0; vertex < positions.GetSize(); ++vertex)
        {
            const math::Vector3 position = positions[vertex];
            const math::Vector3 normal = normals[vertex];
            const math::Vector2 uv = texcoords[vertex];
            inoutStream << position.X << position.Y << position.Z << normal.X << normal.Y << normal.Z << uv.X << uv.Y;
        }
        for (usize i = 0; i < indices.GetSize(); ++i)
        {
            inoutStream << indices[i];
        }
        return true;
    }
    for (usize triangle = 0; triangle < indices.GetSize() / 3; ++triangle)
    {
        const math::Vector3 cross =
            math::Cross(positions[indices[triangle * 3 + 1]] - positions[indices[triangle * 3]],
                        positions[indices[triangle * 3 + 2]] - positions[indices[triangle * 3]]);
        if (math::IsFinite(cross) == false)
        {
            return Fail(outError, QStringLiteral("Schema mesh triangle area is not representable."));
        }
        math::Vector3 faceNormal;
        if (math::TryNormalize(cross, faceNormal) != math::MathStatus::Success)
        {
            // A collapsed (zero-area) triangle covers no pixels and has no face
            // normal; any unit normal is inert. The static bake skips and reports it
            // instead of rejecting the World.
            faceNormal = {0, 0, 1};
        }
        for (uint32 corner = 0; corner < 3; ++corner)
        {
            const uint32 index = indices[triangle * 3 + corner];
            const math::Vector3 position = positions[index];
            const math::Vector2 uv = texcoords[index];
            inoutStream << position.X << position.Y << position.Z << faceNormal.X << faceNormal.Y << faceNormal.Z
                        << uv.X << uv.Y;
        }
    }
    for (usize i = 0; i < indices.GetSize(); ++i)
    {
        inoutStream << static_cast<uint32>(i);
    }
    return true;
}
bool Read(const QString& root,
          const QString& relative,
          QByteArray& outBytes,
          QString& outError,
          int64 maximumBytes = static_cast<int64>(MAX_PREVIEW_DOCUMENT_BYTES))
{
    const QString path = QDir(root).absoluteFilePath(relative);
    const QFileInfo info(path);
    if (relative.isEmpty() == true || QDir::isAbsolutePath(relative) == true ||
        relative.split(QLatin1Char('/')).contains(QStringLiteral("..")) == true ||
        relative.contains(QChar(0)) == true || info.isFile() == false || info.isSymLink() == true ||
        info.canonicalFilePath() != info.absoluteFilePath() || path.startsWith(root + QLatin1Char('/')) == false)
    {
        return Fail(outError, QStringLiteral("Asset must be a regular file inside the project: %1").arg(relative));
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false || file.size() <= 0 || file.size() > maximumBytes)
    {
        return Fail(outError,
                    QStringLiteral("Missing, empty or oversized preview file (%1 bytes maximum): %2")
                        .arg(maximumBytes)
                        .arg(relative));
    }
    outBytes = file.read(maximumBytes + 1);
    return (file.error() == QFileDevice::NoError && outBytes.size() == file.size() &&
            outBytes.size() <= maximumBytes) ||
           Fail(outError, QStringLiteral("Preview file changed or could not be read: %1").arg(relative));
}
bool Decode(const QByteArray& bytes, parsing::JsonDocument& document, QString& error)
{
    parsing::ParseError diagnostic;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_PREVIEW_DOCUMENT_BYTES;
    limits.MaxArrayElements = 12288;
    limits.MaxObjectMembers = 64;
    limits.MaxValues = 65536;
    limits.MaxStringBytes = 1024;
    if (document.Read({bytes.constData(), static_cast<usize>(bytes.size())}, diagnostic, limits) !=
            parsing::ParseStatus::Ok ||
        document.Root().Object() == false)
    {
        return Fail(error, QStringLiteral("Invalid preview JSON, duplicate keys or document limits exceeded."));
    }
    return true;
}
// Only the Ludus-owned parser/conversion API reads text. Qt values are private
// presentation copies; no new Qt/std text-to-number parser is introduced.
bool CopyJson(parsing::JsonValue source, QJsonValue& output)
{
    if (source.Object())
    {
        QJsonObject object;
        for (usize i = 0; i < source.MemberCount(); ++i)
        {
            std::string_view key;
            parsing::JsonValue value;
            QJsonValue copy;
            if (source.MemberAt(i, key, value) == false || CopyJson(value, copy) == false)
            {
                return false;
            }
            object.insert(QString::fromUtf8(key.data(), static_cast<isize>(key.size())), copy);
        }
        output = object;
        return true;
    }
    if (source.Array())
    {
        QJsonArray array;
        for (usize i = 0; i < source.Count(); ++i)
        {
            QJsonValue copy;
            if (CopyJson(source.At(i), copy) == false)
            {
                return false;
            }
            array.append(copy);
        }
        output = array;
        return true;
    }
    if (source.Null())
    {
        output = QJsonValue(QJsonValue::Null);
        return true;
    }
    std::string_view string;
    bool boolean = false;
    float64 number = 0;
    if (source.String(string))
    {
        output = QString::fromUtf8(string.data(), static_cast<isize>(string.size()));
        return true;
    }
    if (source.Boolean(boolean))
    {
        output = boolean;
        return true;
    }
    if (source.Number(number) && math::IsFinite(number))
    {
        output = number;
        return true;
    }
    return false;
}
bool Object(const QByteArray& bytes, QJsonObject& output, QString& error)
{
    parsing::JsonDocument document;
    QJsonValue copy;
    if (Decode(bytes, document, error) == false || CopyJson(document.Root(), copy) == false)
    {
        return Fail(error, error.isEmpty() ? QStringLiteral("Preview values could not be copied.") : error);
    }
    output = copy.toObject();
    return true;
}
bool Fields(const QJsonObject& object, const QStringList& fields)
{
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (fields.contains(it.key()) == false)
        {
            return false;
        }
    }
    return object.size() == fields.size();
}
bool Number(const QJsonValue& value, float32& output)
{
    if (value.isDouble() == false || math::IsFinite(value.toDouble()) == false || value.toDouble() < -1e6 ||
        value.toDouble() > 1e6)
    {
        return false;
    }
    output = static_cast<float32>(value.toDouble());
    return math::IsFinite(output);
}
bool Mesh(const QByteArray& bytes, PreviewMesh& mesh, QString& error)
{
    parsing::JsonDocument document;
    if (Decode(bytes, document, error) == false)
    {
        return false;
    }
    const auto object = document.Root();
    const auto positions = object.Get("positions"), uvs = object.Get("uvs"), indices = object.Get("indices");
    uint64 version = 0;
    if (object.Fields({"version", "positions", "uvs", "indices"}) == false || object.MemberCount() != 4 ||
        object.Get("version").Integer(version) == false || version != 1 || positions.Array() == false ||
        positions.Count() == 0 || positions.Count() > 4096 || uvs.Array() == false ||
        uvs.Count() != positions.Count() || indices.Array() == false || indices.Count() == 0 ||
        indices.Count() > 12288 || indices.Count() % 3 != 0)
    {
        return Fail(error, QStringLiteral("Mesh needs v1, 1–4096 XYZ positions, matching UVs and triangle indices."));
    }
    const auto number = [](parsing::JsonValue source, float32& output) {
        float64 value = 0;
        if (source.Number(value) == false || math::IsFinite(value) == false || value < -1e6 || value > 1e6)
        {
            return false;
        }
        output = static_cast<float32>(value);
        return math::IsFinite(output);
    };
    for (usize i = 0; i < positions.Count(); ++i)
    {
        const auto xyz = positions.At(i), uv = uvs.At(i);
        math::Vector3 p;
        math::Vector2 t;
        if (xyz.Array() == false || xyz.Count() != 3 || uv.Array() == false || uv.Count() != 2 ||
            number(xyz.At(0), p.X) == false || number(xyz.At(1), p.Y) == false || number(xyz.At(2), p.Z) == false ||
            number(uv.At(0), t.X) == false || number(uv.At(1), t.Y) == false)
        {
            return Fail(error, QStringLiteral("Mesh positions/UVs must be finite numbers within ±1,000,000."));
        }
        mesh.Positions.append(p);
        mesh.Uvs.append(t);
    }
    for (usize i = 0; i < indices.Count(); ++i)
    {
        uint64 index = 0;
        if (indices.At(i).Integer(index) == false || index >= positions.Count())
        {
            return Fail(
                error,
                QStringLiteral("Mesh indices must be nonnegative integer JSON tokens within the vertex array."));
        }
        mesh.Indices.append(static_cast<uint32>(index));
    }
    return true;
}
bool Binding(const WorldDocument& world,
             const QJsonObject& descriptor,
             const QString& role,
             const QStringList& names,
             const QStringList& types,
             QJsonObject& output,
             QString& error)
{
    const auto binding = descriptor.value(role).toObject();
    const auto component = world.Component(binding.value(QStringLiteral("component")).toString());
    const auto properties = binding.value(QStringLiteral("properties")).toObject();
    if (Fields(binding, {QStringLiteral("component"), QStringLiteral("properties")}) == false || component.isEmpty() ||
        Fields(properties, names) == false)
    {
        return Fail(error,
                    QStringLiteral("%1 binding must name a declared component and its semantic properties.").arg(role));
    }
    QSet<QString> used;
    for (int32 i = 0; i < names.size(); ++i)
    {
        const auto key = properties.value(names[i]).toString();
        bool found = false;
        for (const auto value : component.value(QStringLiteral("properties")).toArray())
        {
            const auto p = value.toObject();
            if (p.value(QStringLiteral("id")) == QJsonValue(key) &&
                p.value(QStringLiteral("type")) == QJsonValue(types[i]))
            {
                found = true;
            }
        }
        if (key.isEmpty() || used.contains(key) || found == false)
        {
            return Fail(
                error,
                QStringLiteral("%1.%2 must map to a unique property of type %3.").arg(role, names[i], types[i]));
        }
        used.insert(key);
    }
    output = binding;
    return true;
}
QJsonObject Values(const QJsonObject& entity, const QJsonObject& binding)
{
    const auto component = entity.value(QStringLiteral("components"))
                               .toObject()
                               .value(binding.value(QStringLiteral("component")).toString())
                               .toObject();
    QJsonObject values;
    const auto properties = binding.value(QStringLiteral("properties")).toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it)
    {
        values.insert(it.key(), component.value(it.value().toString()));
    }
    return values;
}
bool Attached(const QJsonObject& entity, const QJsonObject& binding)
{
    return entity.value(QStringLiteral("components"))
        .toObject()
        .contains(binding.value(QStringLiteral("component")).toString());
}
bool Transform(const QJsonObject& values, math::TransformTRS& output, QString& error)
{
    math::TransformTRS candidate;
    float32 x = 0, y = 0, z = 0;
    math::Quaternion qx, qy, qz;
    const auto get = [&](const char* key, float32& v) { return Number(values.value(QString::fromLatin1(key)), v); };
    if (get("x", candidate.Translation.X) == false || get("y", candidate.Translation.Y) == false ||
        get("z", candidate.Translation.Z) == false || get("sx", candidate.Scale.X) == false ||
        get("sy", candidate.Scale.Y) == false || get("sz", candidate.Scale.Z) == false || candidate.Scale.X <= 0 ||
        candidate.Scale.Y <= 0 || candidate.Scale.Z <= 0 || get("rx", x) == false || get("ry", y) == false ||
        get("rz", z) == false ||
        math::TryFromAxisAngle({1, 0, 0}, x * 0.01745329252F, qx) != math::MathStatus::Success ||
        math::TryFromAxisAngle({0, 1, 0}, y * 0.01745329252F, qy) != math::MathStatus::Success ||
        math::TryFromAxisAngle({0, 0, 1}, z * 0.01745329252F, qz) != math::MathStatus::Success ||
        math::TryNormalize(qz * qy * qx, candidate.Rotation) != math::MathStatus::Success)
    {
        return Fail(error, QStringLiteral("Preview transform needs finite XYZ/degree rotations and positive scales."));
    }
    output = candidate;
    return true;
}
template <class T>
int32 Find(const QVector<T>& values, const QString& id)
{
    for (int32 i = 0; i < values.size(); ++i)
    {
        if (values[i].Id == id)
        {
            return i;
        }
    }
    return -1;
}
} // namespace
bool RenderScene::Load(const QString& root, const WorldDocument& world, RenderScene& output, QString& error)
{
    error.clear();
    const auto canonical = QFileInfo(root).canonicalFilePath();
    QByteArray bytes;
    QJsonObject descriptor;
    if (world.Loaded() == false || canonical.isEmpty() ||
        Read(canonical, QStringLiteral("authoring/render.preview.json"), bytes, error) == false ||
        Object(bytes, descriptor, error) == false)
    {
        return Fail(error,
                    error.isEmpty() ? QStringLiteral("Open a world in a project with authoring/render.preview.json.")
                                    : error);
    }
    QJsonObject requiredDescriptor = descriptor;
    requiredDescriptor.remove(QStringLiteral("light"));
    if (Fields(requiredDescriptor,
               {QStringLiteral("version"),
                QStringLiteral("schema"),
                QStringLiteral("transform"),
                QStringLiteral("renderable"),
                QStringLiteral("camera"),
                QStringLiteral("meshes"),
                QStringLiteral("textures")}) == false ||
        descriptor.value(QStringLiteral("version")) != QJsonValue(1) ||
        descriptor.value(QStringLiteral("schema")) != world.Schema().value(QStringLiteral("id")) ||
        descriptor.value(QStringLiteral("meshes")).isArray() == false ||
        descriptor.value(QStringLiteral("textures")).isArray() == false)
    {
        return Fail(error,
                    QStringLiteral("Preview descriptor must use v1, this world schema and explicit bindings/assets."));
    }
    QJsonObject transform;
    QJsonObject renderable;
    QJsonObject cameraBinding;
    QJsonObject lightBinding;
    const QString number = QStringLiteral("number");
    if (Binding(world,
                descriptor,
                QStringLiteral("transform"),
                {QStringLiteral("x"),
                 QStringLiteral("y"),
                 QStringLiteral("z"),
                 QStringLiteral("rx"),
                 QStringLiteral("ry"),
                 QStringLiteral("rz"),
                 QStringLiteral("sx"),
                 QStringLiteral("sy"),
                 QStringLiteral("sz")},
                {number, number, number, number, number, number, number, number, number},
                transform,
                error) == false ||
        Binding(world,
                descriptor,
                QStringLiteral("renderable"),
                {QStringLiteral("mesh"),
                 QStringLiteral("texture"),
                 QStringLiteral("visible"),
                 QStringLiteral("r"),
                 QStringLiteral("g"),
                 QStringLiteral("b")},
                {QStringLiteral("asset"), QStringLiteral("asset"), QStringLiteral("boolean"), number, number, number},
                renderable,
                error) == false ||
        (descriptor.value(QStringLiteral("camera")).isNull() == false &&
         Binding(world,
                 descriptor,
                 QStringLiteral("camera"),
                 {QStringLiteral("projection"),
                  QStringLiteral("fov"),
                  QStringLiteral("span"),
                  QStringLiteral("near"),
                  QStringLiteral("far"),
                  QStringLiteral("follow")},
                 {QStringLiteral("enum"), number, number, number, number, QStringLiteral("entity")},
                 cameraBinding,
                 error) == false))
    {
        return false;
    }
    if (descriptor.contains(QStringLiteral("light")) == true &&
        descriptor.value(QStringLiteral("light")).isNull() == false &&
        Binding(
            world,
            descriptor,
            QStringLiteral("light"),
            {QStringLiteral("kind"),
             QStringLiteral("enabled"),
             QStringLiteral("r"),
             QStringLiteral("g"),
             QStringLiteral("b"),
             QStringLiteral("intensity"),
             QStringLiteral("range"),
             QStringLiteral("innerAngle"),
             QStringLiteral("outerAngle")},
            {QStringLiteral("enum"), QStringLiteral("boolean"), number, number, number, number, number, number, number},
            lightBinding,
            error) == false)
    {
        return false;
    }
    RenderScene candidate;
    candidate.RenderableBinding = renderable;
    QByteArray identity = world.Encode() + bytes;
    QByteArray assetsIdentity;
    const auto meshes = descriptor.value(QStringLiteral("meshes")).toArray();
    const auto textures = descriptor.value(QStringLiteral("textures")).toArray();
    if (meshes.isEmpty() || meshes.size() > 6 || textures.size() > 8)
    {
        return Fail(error, QStringLiteral("Preview supports 1–6 meshes and 0–8 PNG textures."));
    }
    for (const bool texture : {false, true})
    {
        QSet<QString> seen;
        for (const auto value : texture ? textures : meshes)
        {
            const auto entry = value.toObject();
            const auto id = entry.value(QStringLiteral("id")).toString();
            bool declared = false;
            for (const auto asset : world.Schema().value(QStringLiteral("assets")).toArray())
            {
                const auto a = asset.toObject();
                declared = declared || (a.value(QStringLiteral("id")) == QJsonValue(id) &&
                                        a.value(QStringLiteral("kind")) ==
                                            QJsonValue(texture ? QStringLiteral("texture") : QStringLiteral("mesh")));
            }
            if (Fields(entry, {QStringLiteral("id"), QStringLiteral("path")}) == false || declared == false ||
                seen.contains(id) == true ||
                Read(canonical,
                     entry.value(QStringLiteral("path")).toString(),
                     bytes,
                     error,
                     static_cast<int64>(texture == true ? MAX_PREVIEW_TEXTURE_FILE_BYTES
                                                        : MAX_PREVIEW_DOCUMENT_BYTES)) == false)
            {
                return Fail(
                    error,
                    QStringLiteral("%1: invalid/duplicate asset or unreadable project file. %2").arg(id, error));
            }
            seen.insert(id);
            assetsIdentity += texture ? 'T' : 'M';
            assetsIdentity += id.toUtf8() + '\0' + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
            if (texture)
            {
                QBuffer buffer(&bytes);
                (void)buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer, QByteArrayLiteral("png"));
                const auto size = reader.size();
                if (size.isValid() == false || size.width() > 2048 || size.height() > 2048)
                {
                    return Fail(error, QStringLiteral("%1: PNG preview must be 1–2048 pixels per axis.").arg(id));
                }
                auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
                if (image.isNull() || image.size() != size)
                {
                    return Fail(error, QStringLiteral("%1: PNG could not be decoded.").arg(id));
                }
                candidate.Textures.append({id, image});
            }
            else
            {
                PreviewMesh mesh;
                mesh.Id = id;
                if (Mesh(bytes, mesh, error) == false)
                {
                    error = id + QStringLiteral(": ") + error;
                    return false;
                }
                candidate.Meshes.append(mesh);
            }
        }
    }
    bool bounds = false;
    bool hasAuthoredLighting = false;
    math::Vector3 low{}, high{};
    for (const auto value : world.Draft().value(QStringLiteral("entities")).toArray())
    {
        const auto entity = value.toObject();
        if (entity.contains(QStringLiteral("scene")))
        {
            continue;
        }
        if (Attached(entity, renderable) == false && Attached(entity, cameraBinding) == false &&
            Attached(entity, lightBinding) == false)
        {
            continue;
        }
        // The entity key: a version-2 UUID or a version-1 ID.
        const QString id = world.GetEntityKey(entity);
        const auto name = entity.value(QStringLiteral("name")).toString();
        math::TransformTRS trs;
        if (Attached(entity, transform) == false || Transform(Values(entity, transform), trs, error) == false)
        {
            return Fail(error, name + QStringLiteral(": missing or invalid transform. ") + error);
        }
        if (Attached(entity, lightBinding) == true)
        {
            hasAuthoredLighting = true;
            const QJsonObject values = Values(entity, lightBinding);
            if (values.value(QStringLiteral("enabled")).toBool() == true)
            {
                rr::DirectLight light;
                const QString kind = values.value(QStringLiteral("kind")).toString();
                light.Kind = kind == QStringLiteral("directional") ? rr::LightKind::Directional
                             : kind == QStringLiteral("point")     ? rr::LightKind::Point
                                                                   : rr::LightKind::Spot;
                float32 innerDegrees = 0;
                float32 outerDegrees = 0;
                if ((kind != QStringLiteral("directional") && kind != QStringLiteral("point") &&
                     kind != QStringLiteral("spot")) ||
                    Number(values.value(QStringLiteral("r")), light.Color.X) == false ||
                    Number(values.value(QStringLiteral("g")), light.Color.Y) == false ||
                    Number(values.value(QStringLiteral("b")), light.Color.Z) == false ||
                    Number(values.value(QStringLiteral("intensity")), light.Intensity) == false ||
                    Number(values.value(QStringLiteral("range")), light.Range) == false ||
                    Number(values.value(QStringLiteral("innerAngle")), innerDegrees) == false ||
                    Number(values.value(QStringLiteral("outerAngle")), outerDegrees) == false || innerDegrees < 0 ||
                    innerDegrees >= outerDegrees || outerDegrees >= 90 || trs.Scale != math::Vector3{1, 1, 1})
                {
                    return Fail(error, name + QStringLiteral(": invalid light terms, cone or non-unit scale."));
                }
                light.Position = trs.Translation;
                light.Direction = math::Rotate(trs.Rotation, {0, 0, -1});
                light.InnerCos = math::Cos(math::DegreesToRadians(innerDegrees));
                light.OuterCos = math::Cos(math::DegreesToRadians(outerDegrees));
                light.SourceId = ComputeSourceId(id);
                if (candidate.AuthoredLights.Count >= sizeof(candidate.AuthoredLights.Lights) / sizeof(rr::DirectLight))
                {
                    return Fail(error, QStringLiteral("World exceeds the authored-light capacity."));
                }
                candidate.AuthoredLights.Lights[candidate.AuthoredLights.Count++] = light;
                assetsIdentity += QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                                            {QStringLiteral("light"), values},
                                                            {QStringLiteral("transform"), Values(entity, transform)}})
                                      .toJson(QJsonDocument::Compact);
            }
        }
        if (Attached(entity, renderable))
        {
            const auto values = Values(entity, renderable);
            if (values.value(QStringLiteral("visible")).toBool())
            {
                PreviewObject object;
                object.Id = id;
                object.Name = name;
                object.Mesh = Find(candidate.Meshes, values.value(QStringLiteral("mesh")).toString());
                const auto textureId = values.value(QStringLiteral("texture")).toString();
                object.Texture = textureId.isEmpty() ? -1 : Find(candidate.Textures, textureId);
                if (object.Mesh < 0 || (textureId.isEmpty() == false && object.Texture < 0) ||
                    Number(values.value(QStringLiteral("r")), object.Tint.X) == false ||
                    Number(values.value(QStringLiteral("g")), object.Tint.Y) == false ||
                    Number(values.value(QStringLiteral("b")), object.Tint.Z) == false || object.Tint.X < 0 ||
                    object.Tint.X > 1 || object.Tint.Y < 0 || object.Tint.Y > 1 || object.Tint.Z < 0 ||
                    object.Tint.Z > 1 || math::TryToAffine3(trs, 1e-4F, object.Transform) != math::MathStatus::Success)
                {
                    return Fail(error, name + QStringLiteral(": choose mapped mesh/texture, RGB in [0,1]."));
                }
                if (object.Texture >= 0)
                {
                    for (int32 i = 0; i < candidate.Surfaces.size(); ++i)
                    {
                        if (candidate.Surfaces[i].Texture == object.Texture &&
                            candidate.Surfaces[i].Tint == object.Tint)
                        {
                            object.Surface = i;
                            break;
                        }
                    }
                    if (object.Surface < 0)
                    {
                        if (candidate.Surfaces.size() == 8)
                        {
                            return Fail(error,
                                        QStringLiteral("Preview supports eight distinct texture/tint combinations; "
                                                       "identical surfaces are shared."));
                        }
                        object.Surface = static_cast<int32>(candidate.Surfaces.size());
                        candidate.Surfaces.append(
                            { .Texture = object.Texture, .Tint = object.Tint, .FlatTint = object.Tint });
                    }
                }
                for (const auto vertex : candidate.Meshes[object.Mesh].Positions)
                {
                    const auto p = math::TransformPoint(object.Transform, vertex);
                    if (math::IsFinite(p) == false || math::Abs(p.X) > 1e6F || math::Abs(p.Y) > 1e6F ||
                        math::Abs(p.Z) > 1e6F)
                    {
                        return Fail(error, name + QStringLiteral(": transformed geometry exceeds preview bounds."));
                    }
                    if (bounds == false)
                    {
                        low = high = p;
                        bounds = true;
                    }
                    low = {math::Min(low.X, p.X), math::Min(low.Y, p.Y), math::Min(low.Z, p.Z)};
                    high = {math::Max(high.X, p.X), math::Max(high.Y, p.Y), math::Max(high.Z, p.Z)};
                }
                candidate.Objects.append(object);
            }
        }
        if (Attached(entity, cameraBinding))
        {
            const auto values = Values(entity, cameraBinding);
            if (trs.Scale != math::Vector3{1, 1, 1})
            {
                return Fail(error, name + QStringLiteral(": cameras require unit scale (a rigid pose)."));
            }
            camera::CameraRigDefinition definition;
            const auto authoredPose = Values(entity, transform);
            definition.Pose.Position = {authoredPose.value(QStringLiteral("x")).toDouble(),
                                        authoredPose.value(QStringLiteral("y")).toDouble(),
                                        authoredPose.value(QStringLiteral("z")).toDouble()};
            definition.Pose.Orientation = trs.Rotation;
            const auto projection = values.value(QStringLiteral("projection")).toString();
            definition.Lens.Projection =
                projection == QStringLiteral("perspective") ? camera::CameraProjection::PerspectiveFinite
                : projection == QStringLiteral("infinite")  ? camera::CameraProjection::PerspectiveInfinite
                                                            : camera::CameraProjection::OrthographicFinite;
            if ((projection != QStringLiteral("perspective") && projection != QStringLiteral("infinite") &&
                 projection != QStringLiteral("orthographic")) ||
                Number(values.value(QStringLiteral("fov")), definition.Lens.VerticalFov) == false ||
                Number(values.value(QStringLiteral("span")), definition.Lens.VerticalSpan) == false ||
                Number(values.value(QStringLiteral("near")), definition.Lens.NearPlane) == false ||
                Number(values.value(QStringLiteral("far")), definition.Lens.FarPlane) == false)
            {
                return Fail(error, name + QStringLiteral(": invalid camera lens."));
            }
            definition.Lens.VerticalFov *= 0.01745329252F;
            camera::CameraTargetSample target;
            const auto follow = values.value(QStringLiteral("follow")).toString();
            if (follow.isEmpty() == false)
            {
                const auto e = world.Entity(follow);
                math::TransformTRS targetTransform;
                if (Attached(e, transform) == false || Transform(Values(e, transform), targetTransform, error) == false)
                {
                    return Fail(error, name + QStringLiteral(": follow target needs a valid transform."));
                }
                definition.Recipe = camera::CameraRecipe::OrthographicFollow;
                const auto targetValues = Values(e, transform);
                target.Position = {targetValues.value(QStringLiteral("x")).toDouble(),
                                   targetValues.value(QStringLiteral("y")).toDouble(),
                                   targetValues.value(QStringLiteral("z")).toDouble()};
            }
            PreviewCamera rig;
            rig.Id = id;
            rig.Name = name;
            rig.FollowId = follow;
            if (camera::TryEvaluateCamera(definition,
                                          {},
                                          follow.isEmpty() ? nullptr : &target,
                                          rig.Sample,
                                          &rig.Trace) != camera::CameraStatus::Success)
            {
                return Fail(error, name + QStringLiteral(": lens/follow is unsupported by the C0 camera contract."));
            }
            candidate.Cameras.append(rig);
        }
    }
    rr::LightSelection normalized;
    if (rr::NormalizeDirectLights(candidate.AuthoredLights.Lights, candidate.AuthoredLights.Count, normalized) !=
        graphics::rhi::RasterStatus::Ready)
    {
        return Fail(error, QStringLiteral("Invalid light color/intensity/range or conflicting source identity."));
    }
    candidate.AuthoredLights = normalized;
    candidate.Center = (low + high) * 0.5F;
    candidate.Radius = bounds ? math::Max(0.001F, math::Length(high - low) * 0.5F) : 1.0F;
    candidate.AssetsIdentity = QCryptographicHash::hash(assetsIdentity, QCryptographicHash::Sha256);
    candidate.Identity = QCryptographicHash::hash(identity + candidate.AssetsIdentity, QCryptographicHash::Sha256);
    if (hasAuthoredLighting == true && candidate.Objects.isEmpty() == false)
    {
        const math::Vector3 eye = candidate.Center + math::Vector3{0, 0, candidate.Radius * 3};
        const QJsonObject camera{
            {QStringLiteral("eye"),
             QJsonArray{static_cast<float64>(eye.X), static_cast<float64>(eye.Y), static_cast<float64>(eye.Z)}},
            {QStringLiteral("target"),
             QJsonArray{static_cast<float64>(candidate.Center.X),
                        static_cast<float64>(candidate.Center.Y),
                        static_cast<float64>(candidate.Center.Z)}},
            {QStringLiteral("up"), QJsonArray{0, 1, 0}},
            {QStringLiteral("fov"), 1.0}};
        const QJsonObject level{{QStringLiteral("id"), QStringLiteral("schema-preview")},
                                {QStringLiteral("objects"), QJsonArray{}},
                                {QStringLiteral("camera"), camera}};
        return Merge(candidate, {}, level, output, error, {});
    }
    output = foundation::Move(candidate);
    error.clear();
    return true;
}
bool RenderScene::TryGetBounds(const QString& id, math::Aabb3& outBoundsWorld) const noexcept
{
    math::Aabb3 candidate = math::EmptyAabb3();
    bool hasPoint = false;
    if (Definition != nullptr && Definition->Meshes.GetSize() != static_cast<usize>(Meshes.size()))
    {
        return false;
    }
    for (const PreviewObject& object : Objects)
    {
        if (id.isEmpty() == false && object.Id != id)
        {
            continue;
        }
        if (object.Mesh < 0 || object.Mesh >= Meshes.size())
        {
            return false;
        }
        const BorrowedView<const math::Vector3> positions = GetMeshPositions(*this, object.Mesh);
        for (usize i = 0; i < positions.GetSize(); ++i)
        {
            const math::Vector3 positionMesh = positions[i];
            const math::Vector3 positionWorld = math::TransformPoint(object.Transform, positionMesh);
            if (math::IsFinite(positionWorld) == false)
            {
                return false;
            }
            candidate.Min = {math::Min(candidate.Min.X, positionWorld.X),
                             math::Min(candidate.Min.Y, positionWorld.Y),
                             math::Min(candidate.Min.Z, positionWorld.Z)};
            candidate.Max = {math::Max(candidate.Max.X, positionWorld.X),
                             math::Max(candidate.Max.Y, positionWorld.Y),
                             math::Max(candidate.Max.Z, positionWorld.Z)};
            hasPoint = true;
        }
    }
    if (hasPoint == false)
    {
        return false;
    }
    outBoundsWorld = candidate;
    return true;
}
bool RenderScene::View(const PreviewViewOptions& options, rr::ViewDescription& outView, QString& outError) const
{
    const int32 selected = options.Camera;
    const float32 yaw = options.OrbitDegrees.X;
    const float32 pitch = options.OrbitDegrees.Y;
    const float32 distance = options.Distance;
    const uint32 width = options.Width;
    const uint32 height = options.Height;
    const bool isReference = options.Reference;
    const math::Vector3* orbitCenterOrNull = options.OrbitCenter;
    const float32 orbitRadius = options.OrbitRadius;
    rr::ViewMapping mapping;
    rr::ProjectionDescription projection;
    math::Vector3 position;
    math::Vector3 forward;
    if (selected < -1 || selected >= Cameras.size() ||
        rr::ResolveViewMapping({static_cast<float32>(width), static_cast<float32>(height), rr::ScreenFit::Stretch},
                               {0, 0, width, height},
                               width,
                               height,
                               mapping) != graphics::rhi::RasterStatus::Ready)
    {
        return Fail(outError, QStringLiteral("Camera selection or drawable dimensions are unavailable."));
    }
    if (selected == -1 && options.EditorCameraOrNull == nullptr)
    {
        const math::Vector3 center = orbitCenterOrNull == nullptr ? Center : *orbitCenterOrNull;
        const float32 radius = orbitCenterOrNull == nullptr ? Radius : orbitRadius;
        if (math::IsFinite(yaw) == false || math::IsFinite(pitch) == false || math::IsFinite(distance) == false ||
            distance <= 0 || distance > 10000000 || pitch < -89.9F || pitch > 89.9F)
        {
            return Fail(outError, QStringLiteral("Editor orbit is outside its safe navigation range."));
        }
        const float32 y = math::DegreesToRadians(yaw);
        const float32 p = math::DegreesToRadians(pitch);
        position =
            center + math::Vector3{math::Sin(y) * math::Cos(p), math::Sin(p), math::Cos(y) * math::Cos(p)} * distance;
        if (math::TryLookAt(position, center, {0, 1, 0}, projection.WorldToView) != math::MathStatus::Success ||
            math::TryNormalize(center - position, forward) != math::MathStatus::Success)
        {
            return Fail(outError, QStringLiteral("Editor orbit cannot produce a valid view."));
        }
        projection.Kind = rr::ProjectionKind::PerspectiveInfinite;
        projection.Near = math::Max(0.000001F, radius * 0.001F);
    }
    else
    {
        const camera::CameraSample& sample = selected == -1 ? *options.EditorCameraOrNull : Cameras[selected].Sample;
        if (camera::ValidateCameraSample(sample) != camera::CameraStatus::Success)
        {
            return Fail(outError, QStringLiteral("Editor camera publication is invalid."));
        }
        math::TransformTRS pose;
        if (math::TryMakeRelative(sample.Pose.Position, {}, 10000000.0, pose.Translation) != math::MathStatus::Success)
        {
            return Fail(outError, QStringLiteral("Camera position exceeds the renderer's origin-zero range."));
        }
        pose.Rotation = sample.Pose.Orientation;
        math::Affine3 inverse;
        if (math::TryInverse(math::ToAffine3(pose), inverse) != math::MathStatus::Success)
        {
            return Fail(outError, QStringLiteral("Authored camera pose cannot be inverted."));
        }
        projection.WorldToView = math::ToMatrix4(inverse);
        projection.Kind = sample.Lens.Projection == camera::CameraProjection::PerspectiveFinite
                              ? rr::ProjectionKind::Perspective
                          : sample.Lens.Projection == camera::CameraProjection::PerspectiveInfinite
                              ? rr::ProjectionKind::PerspectiveInfinite
                              : rr::ProjectionKind::Orthographic;
        projection.VerticalFovRadians = sample.Lens.VerticalFov;
        projection.OrthographicHeight =
            math::Max(sample.Lens.VerticalSpan,
                      sample.Lens.MinimumVisibleWidth * static_cast<float32>(height) / static_cast<float32>(width));
        projection.Near = sample.Lens.NearPlane;
        projection.Far = sample.Lens.FarPlane;
        position = pose.Translation;
        forward = math::Rotate(pose.Rotation, {0, 0, -1});
    }
    if (options.ShouldUseConventionalDepth == true && projection.Kind == rr::ProjectionKind::PerspectiveInfinite)
    {
        // The conventional flip is exact only for a finite projection, so bound the far plane by
        // the farthest corner of the World's geometry from this camera.
        math::Aabb3 boundsWorld;
        if (TryGetBounds({}, boundsWorld) == false)
        {
            return Fail(outError, QStringLiteral("The World has no finite geometry bounds for a finite projection."));
        }
        float32 farthest = 0;
        for (usize corner = 0; corner < AABB_CORNER_COUNT; ++corner)
        {
            const math::Vector3 cornerWorld{(corner & 1U) != 0 ? boundsWorld.Max.X : boundsWorld.Min.X,
                                            (corner & 2U) != 0 ? boundsWorld.Max.Y : boundsWorld.Min.Y,
                                            (corner & 4U) != 0 ? boundsWorld.Max.Z : boundsWorld.Min.Z};
            farthest = math::Max(farthest, math::Length(cornerWorld - position));
        }
        projection.Kind = rr::ProjectionKind::Perspective;
        projection.Far = math::Max(farthest * FINITE_FAR_MARGIN, projection.Near * MIN_FAR_TO_NEAR);
    }
    rr::ViewDescription view;
    if (rr::BuildViewDescription(projection, mapping, view) != graphics::rhi::RasterStatus::Ready)
    {
        return Fail(outError, QStringLiteral("Camera lens cannot be represented by the renderer."));
    }
    if (options.ShouldUseConventionalDepth == true)
    {
        ConvertToConventionalDepth(view);
    }
    view.CameraPosition = position;
    view.CameraForward = forward;
    view.OrthographicCamera = projection.Kind == rr::ProjectionKind::Orthographic;
    view.Reference = isReference;
    outView = view;
    outError.clear();
    return true;
}
bool RenderScene::Merge(const RenderScene& authored,
                        const QByteArray& cooked,
                        const QJsonObject& level,
                        RenderScene& output,
                        QString& error,
                        const QJsonObject& revisions)
{
    if (authored.Objects.isEmpty() == true && authored.AuthoredLights.Count == 0)
    {
        if (cooked.isEmpty() == false)
        {
            return LoadCooked(cooked, level, output, error, revisions);
        }
        output = {};
        error.clear();
        return true;
    }
    resources::SceneDefinition imported;
    if (cooked.isEmpty() == false &&
        resources::SceneDefinition::Decode(reinterpret_cast<const uint8*>(cooked.constData()),
                                           static_cast<usize>(cooked.size()),
                                           imported) != resources::Status::Ok)
    {
        return Fail(error, QStringLiteral("Invalid imported scene during World composition."));
    }
    const usize meshCount = imported.Meshes.GetSize() +
                            (authored.Objects.isEmpty() == true ? 0 : static_cast<usize>(authored.Meshes.size()));
    const usize materialCount = imported.Materials.GetSize() + static_cast<usize>(authored.Objects.size());
    const usize textureCount = imported.Textures.GetSize() + static_cast<usize>(authored.Objects.size());
    const usize objectCount = imported.Objects.GetSize() + static_cast<usize>(authored.Objects.size());
    if (meshCount > format::MAX_MESHES || materialCount > format::MAX_MATERIALS ||
        textureCount > format::MAX_TEXTURES || objectCount > format::MAX_OBJECTS)
    {
        return Fail(
            error,
            QStringLiteral("Combined World exceeds six meshes, eight materials/textures or 128 visible instances."));
    }
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream.writeRawData(format::MAGIC.data(), static_cast<int32>(format::MAGIC.size()));
    stream << format::VERSION_4 << static_cast<uint32>(meshCount) << static_cast<uint32>(materialCount)
           << static_cast<uint32>(textureCount) << static_cast<uint32>(objectCount);
    const auto camera = level.value(QStringLiteral("camera")).toObject();
    for (const auto& key : {QStringLiteral("eye"), QStringLiteral("target"), QStringLiteral("up")})
    {
        for (const auto n : camera.value(key).toArray())
        {
            stream << static_cast<float32>(n.toDouble());
        }
    }
    stream << static_cast<float32>(camera.value(QStringLiteral("fov")).toDouble());
    for (const auto& texture : imported.Textures)
    {
        stream << texture.Width << texture.Height << static_cast<uint32>(texture.Pixels.GetSize());
        stream.writeRawData(reinterpret_cast<const char*>(texture.Pixels.GetData()),
                            static_cast<int32>(texture.Pixels.GetSize()));
    }
    // The reference profile has no base-color multiplier. Bake the validated
    // schema tint into transient sRGB pixels; durable source assets stay intact.
    for (const auto& object : authored.Objects)
    {
        QImage image = object.Texture < 0 ? QImage(1, 1, QImage::Format_RGBA8888)
                                          : authored.Textures[object.Texture].Pixels.copy();
        if (image.isNull())
        {
            return Fail(error, QStringLiteral("World texture allocation failed."));
        }
        if (object.Texture < 0)
        {
            image.fill(Qt::white);
        }
        for (int32 y = 0; y < image.height(); ++y)
        {
            auto* pixels = image.scanLine(y);
            for (int32 x = 0; x < image.width(); ++x)
            {
                const float32 tint[3]{object.Tint.X, object.Tint.Y, object.Tint.Z};
                for (int32 channel = 0; channel < 3; ++channel)
                {
                    const auto linear = math::DecodeSrgb(static_cast<float32>(pixels[x * 4 + channel]) / 255.0F);
                    pixels[x * 4 + channel] = static_cast<uint8>(
                        math::Floor(static_cast<float64>(math::EncodeSrgb(linear * tint[channel])) * 255.0 + .5));
                }
            }
        }
        stream << static_cast<uint32>(image.width()) << static_cast<uint32>(image.height())
               << static_cast<uint32>(image.width() * image.height() * 4);
        const int32 rowBytes = image.width() * 4; // Preview textures are bounded to 2048 pixels per axis.
        for (int32 y = 0; y < image.height(); ++y)
        {
            stream.writeRawData(reinterpret_cast<const char*>(image.constScanLine(y)), rowBytes);
        }
    }
    for (const resources::SceneMaterial& material : imported.Materials)
    {
        stream << material.Texture << uint32{material.Repeat ? 1U : 0U} << material.Metallic << material.Roughness
               << material.Emission.X << material.Emission.Y << material.Emission.Z;
        const uint32 flags = (material.IsEmissionTwoSided == true ? format::EMISSION_TWO_SIDED : 0U) |
                             (material.HasEmissionPower == true ? format::EMISSION_HAS_POWER : 0U);
        stream << flags;
        WriteLittleEndianUint64(stream, material.AreaSourceId);
        stream << material.EmissionPower << uint32{0};
    }
    for (int32 i = 0; i < authored.Objects.size(); ++i)
    {
        stream << static_cast<uint32>(imported.Textures.GetSize() + static_cast<usize>(i)) << uint32{0} << float32{0}
               << float32{.5F} << float32{0} << float32{0} << float32{0} << uint32{0};
        WriteLittleEndianUint64(stream, 0);
        stream << float32{0} << uint32{0};
    }
    usize vertexCount = 0;
    usize indexCount = 0;
    for (const resources::SceneMesh& mesh : imported.Meshes)
    {
        if (WriteSceneMesh(stream,
                           {mesh.Positions.GetData(), mesh.Positions.GetSize()},
                           {mesh.Normals.GetData(), mesh.Normals.GetSize()},
                           {mesh.Texcoords.GetData(), mesh.Texcoords.GetSize()},
                           {mesh.Indices.GetData(), mesh.Indices.GetSize()},
                           vertexCount,
                           indexCount,
                           error) == false)
        {
            return false;
        }
    }
    if (authored.Objects.isEmpty() == false)
    {
        for (const PreviewMesh& mesh : authored.Meshes)
        {
            if (WriteSceneMesh(stream,
                               {mesh.Positions.constData(), static_cast<usize>(mesh.Positions.size())},
                               {mesh.Normals.constData(), static_cast<usize>(mesh.Normals.size())},
                               {mesh.Uvs.constData(), static_cast<usize>(mesh.Uvs.size())},
                               {mesh.Indices.constData(), static_cast<usize>(mesh.Indices.size())},
                               vertexCount,
                               indexCount,
                               error) == false)
            {
                return false;
            }
        }
    }
    for (const resources::SceneObject& object : imported.Objects)
    {
        stream << object.Mesh << object.Material;
        for (const auto column : object.Transform.Columns)
        {
            stream << column.X << column.Y << column.Z;
        }
        stream << object.Transform.Translation.X << object.Transform.Translation.Y << object.Transform.Translation.Z;
        WriteLittleEndianUint64(stream, object.SourceId);
    }
    QJsonObject mergedLevel = level;
    QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    for (int32 i = 0; i < authored.Objects.size(); ++i)
    {
        const PreviewObject& object = authored.Objects[i];
        stream << static_cast<uint32>(imported.Meshes.GetSize() + static_cast<usize>(object.Mesh))
               << static_cast<uint32>(imported.Materials.GetSize() + static_cast<usize>(i));
        for (const math::Vector3 column : object.Transform.Columns)
        {
            stream << column.X << column.Y << column.Z;
        }
        stream << object.Transform.Translation.X << object.Transform.Translation.Y << object.Transform.Translation.Z;
        WriteLittleEndianUint64(stream, ComputeSourceId(object.Id));
        objects.append(
            QJsonObject{{QStringLiteral("id"), object.Id},
                        {QStringLiteral("name"), object.Name},
                        {QStringLiteral("mesh"), QStringLiteral("preview/") + authored.Meshes[object.Mesh].Id},
                        {QStringLiteral("material"), QStringLiteral("preview/") + object.Id}});
    }
    const usize lightCount = imported.Lights.GetSize() + authored.AuthoredLights.Count;
    if (lightCount > format::MAX_LIGHTS)
    {
        return Fail(error, QStringLiteral("Combined authored lights exceed the scene capacity."));
    }
    stream << format::LIGHTING_PROFILE_REC709_PHOTOMETRIC << static_cast<uint32>(lightCount)
           << imported.Environment.Width << imported.Environment.Height << imported.Environment.Rgb.X
           << imported.Environment.Rgb.Y << imported.Environment.Rgb.Z;
    for (const resources::SceneLight& light : imported.Lights)
    {
        WriteLittleEndianUint64(stream, light.SourceId);
        stream << static_cast<uint32>(light.Kind) << uint32{light.IsEnabled == true ? format::LIGHT_ENABLED : 0}
               << light.IntensityRgb.X << light.IntensityRgb.Y << light.IntensityRgb.Z << light.PositionWorld.X
               << light.PositionWorld.Y << light.PositionWorld.Z << light.DirectionWorld.X << light.DirectionWorld.Y
               << light.DirectionWorld.Z << light.Range << light.InnerCos << light.OuterCos;
    }
    for (usize i = 0; i < authored.AuthoredLights.Count; ++i)
    {
        const rr::DirectLight& light = authored.AuthoredLights.Lights[i];
        const math::Vector3 rgb = light.Color * light.Intensity;
        WriteLittleEndianUint64(stream, light.SourceId);
        stream << static_cast<uint32>(light.Kind) << format::LIGHT_ENABLED << rgb.X << rgb.Y << rgb.Z
               << light.Position.X << light.Position.Y << light.Position.Z << light.Direction.X << light.Direction.Y
               << light.Direction.Z << light.Range << light.InnerCos << light.OuterCos;
    }
    for (const math::Vector3 pixel : imported.Environment.Pixels)
    {
        stream << pixel.X << pixel.Y << pixel.Z;
    }
    mergedLevel.insert(QStringLiteral("objects"), objects);
    if (stream.status() != QDataStream::Ok || bytes.size() > static_cast<qsizetype>(format::MAX_BUNDLE_BYTES))
    {
        return Fail(error, QStringLiteral("Combined World exceeds its cooked byte budget."));
    }
    RenderScene candidate;
    if (LoadCooked(bytes, mergedLevel, candidate, error, revisions) == false)
    {
        return false;
    }
    candidate.AuthoredLights = authored.AuthoredLights;
    candidate.RenderableBinding = authored.RenderableBinding;
    if (authored.Cameras.isEmpty() == false)
    {
        candidate.Cameras = authored.Cameras;
    }
    candidate.AssetsIdentity =
        QCryptographicHash::hash(candidate.AssetsIdentity + authored.AssetsIdentity, QCryptographicHash::Sha256);
    candidate.Identity = QCryptographicHash::hash(candidate.Identity + authored.Identity, QCryptographicHash::Sha256);
    output = foundation::Move(candidate);
    return true;
}
bool RenderScene::LoadCooked(const QByteArray& bytes,
                             const QJsonObject& level,
                             RenderScene& output,
                             QString& error,
                             const QJsonObject& revisions)
{
    RenderScene candidate;
    SharedPtr<resources::SceneDefinition> decoded;
    if (TryMakeShared<resources::SceneDefinition>(GetSystemAllocationDomain(), decoded) != OwnershipStatus::Ok)
    {
        return Fail(error, QStringLiteral("Cooked scene allocation failed. Previous viewport retained."));
    }
    if (resources::SceneDefinition::Decode(reinterpret_cast<const uint8*>(bytes.constData()),
                                           static_cast<usize>(bytes.size()),
                                           *decoded) != resources::Status::Ok)
    {
        return Fail(error,
                    QStringLiteral("Cooked scene failed bounded runtime validation. Previous viewport retained."));
    }
    // Publish only the complete decode; no holder mutates it afterwards.
    candidate.Definition = Move(decoded);
    const auto& scene = *candidate.Definition;
    candidate.Lit = true;
    candidate.LogicalId = QStringLiteral("level/") + level.value(QStringLiteral("id")).toString();
    for (const auto& source : scene.Meshes)
    {
        PreviewMesh mesh;
        (void)source; // Geometry stays in the immutable leased definition.
        candidate.Meshes.append(mesh);
    }
    // One linear mean per texture, shared by every material that samples it.
    QVector<math::Vector3> textureMeans;
    for (const auto& source : scene.Textures)
    {
        textureMeans.append(ComputeLinearMeanRgb(source));
        PreviewTexture texture;
        texture.Pixels = QImage(source.Pixels.GetData(),
                                static_cast<int32>(source.Width),
                                static_cast<int32>(source.Height),
                                QImage::Format_RGBA8888);
        if (texture.Pixels.isNull())
        {
            return Fail(error, QStringLiteral("Texture allocation failed."));
        }
        candidate.Textures.append(texture);
    }
    for (const auto& source : scene.Materials)
    {
        PreviewSurface surface;
        surface.Texture = source.Texture == ~uint32{0} ? -1 : static_cast<int32>(source.Texture);
        surface.Tint = surface.Texture < 0 ? math::Vector4{0, 0, 0, 1} : math::Vector4{1, 1, 1, 1};
        surface.FlatTint = surface.Tint;
        if (surface.Texture >= 0)
        {
            if (surface.Texture >= textureMeans.size())
            {
                return Fail(error, QStringLiteral("Cooked material names a missing texture."));
            }
            const math::Vector3& mean = textureMeans[surface.Texture];
            surface.FlatTint = {surface.Tint.X * mean.X, surface.Tint.Y * mean.Y, surface.Tint.Z * mean.Z, 1};
        }
        surface.Repeat = source.Repeat;
        surface.Metallic = source.Metallic;
        surface.Roughness = source.Roughness;
        surface.Emission = source.Emission;
        candidate.Surfaces.append(surface);
    }
    const auto objects = level.value(QStringLiteral("objects")).toArray();
    if (objects.size() != static_cast<qsizetype>(scene.Objects.GetSize()))
    {
        return Fail(error, QStringLiteral("Level object count mismatch."));
    }
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        const auto& source = scene.Objects[i];
        const auto persistent = objects[static_cast<qsizetype>(i)].toObject();
        PreviewObject object;
        object.Id = persistent.value(QStringLiteral("id")).toString();
        object.Name = persistent.value(QStringLiteral("name")).toString();
        object.Mesh = static_cast<int32>(source.Mesh);
        object.Surface = static_cast<int32>(source.Material);
        object.Texture = candidate.Surfaces[object.Surface].Texture;
        object.Transform = source.Transform;
        candidate.Objects.append(object);
    }
    candidate.PacketIdentity = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    QJsonArray bindings;
    QStringList resourceIds;
    for (const auto& value : objects)
    {
        const auto object = value.toObject();
        const auto mesh = object.value(QStringLiteral("mesh")).toString();
        const auto material = object.value(QStringLiteral("material")).toString();
        bindings.append(QJsonObject{{QStringLiteral("id"), object.value(QStringLiteral("id"))},
                                    {QStringLiteral("mesh"), mesh},
                                    {QStringLiteral("material"), material}});
        if (resourceIds.contains(mesh) == false)
        {
            resourceIds.append(mesh);
        }
        if (resourceIds.contains(material) == false)
        {
            resourceIds.append(material);
        }
    }
    resourceIds.sort();
    const auto identities = QJsonDocument(QJsonArray::fromStringList(resourceIds)).toJson(QJsonDocument::Compact);
    QCryptographicHash assets(QCryptographicHash::Sha256);
    assets.addData(identities);
    assets.addData(QJsonDocument(revisions).toJson(QJsonDocument::Compact));
    candidate.AssetsIdentity = assets.result();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(candidate.PacketIdentity);
    hash.addData(candidate.AssetsIdentity);
    hash.addData(QJsonDocument(QJsonObject{{QStringLiteral("level"), level.value(QStringLiteral("id"))},
                                           {QStringLiteral("bindings"), bindings},
                                           {QStringLiteral("resources"), QJsonArray::fromStringList(resourceIds)}})
                     .toJson(QJsonDocument::Compact));
    candidate.Identity = hash.result();
    output = foundation::Move(candidate);
    error.clear();
    return true;
}
} // namespace ludus::editor
