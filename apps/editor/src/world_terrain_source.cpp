#include "internal/world_terrain_source.h"

#include "internal/project_files.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonValue>
#include <QSize>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr int32 TERRAIN_STROKE_VERSION = 1;
constexpr qsizetype SHA256_HEX_LENGTH = 64;
constexpr qsizetype MAX_HEIGHTMAP_PATH_BYTES = 512;
constexpr qsizetype HEIGHTMAP_FIELDS = 4;
constexpr qsizetype STROKE_FIELDS = 5;
constexpr float64 RAISE_METRES_PER_DAB = 1;

bool RefuseTerrainSource(QString& outError, const QString& message)
{
    outError = message;
    return false;
}

QString GetBrushName(TerrainBrushKind kind)
{
    switch (kind)
    {
        case TerrainBrushKind::Raise:
            return QStringLiteral("raise");
        case TerrainBrushKind::Lower:
            return QStringLiteral("lower");
        case TerrainBrushKind::Smooth:
            return QStringLiteral("smooth");
        case TerrainBrushKind::Flatten:
            return QStringLiteral("flatten");
    }
    return {};
}

bool TryReadBrushKind(const QString& name, TerrainBrushKind& outKind)
{
    for (const TerrainBrushKind kind :
         {TerrainBrushKind::Raise, TerrainBrushKind::Lower, TerrainBrushKind::Smooth, TerrainBrushKind::Flatten})
    {
        if (name == GetBrushName(kind))
        {
            outKind = kind;
            return true;
        }
    }
    return false;
}

bool IsInTerrainRange(const QJsonValue& value, float64 minimum, float64 maximum)
{
    return value.isDouble() == true && math::IsFinite(value.toDouble()) == true && value.toDouble() >= minimum &&
           value.toDouble() <= maximum;
}

bool IsValidHeightmapPath(const QString& relative)
{
    return relative.isEmpty() == false && QDir::isAbsolutePath(relative) == false &&
           relative.toUtf8().size() <= MAX_HEIGHTMAP_PATH_BYTES && QDir::cleanPath(relative) == relative &&
           relative == QDir::fromNativeSeparators(relative) && relative.contains(QLatin1Char('\\')) == false &&
           relative.contains(QChar(0)) == false &&
           relative.split(QLatin1Char('/')).contains(QStringLiteral("..")) == false;
}

// Thanks to the W3C PNG Working Group, Portable Network Graphics (PNG) Specification (Third Edition),
// §§ 5.2–5.3 and 11.2.1 (https://www.w3.org/TR/2025/REC-png-3-20250624/#11IHDR).
// Admit the grayscale IHDR before decoding and forward only image chunks. Height samples use raw code values.
bool ExtractHeightmapPng(const QByteArray& bytes, QByteArray& outPng, QString& outError)
{
    const QByteArray signature = QByteArray::fromHex(QByteArrayLiteral("89504e470d0a1a0a"));
    if (bytes.startsWith(signature) == false)
    {
        return RefuseTerrainSource(outError, QStringLiteral("Choose a grayscale PNG heightmap."));
    }
    constexpr qsizetype CHUNK_TYPE_BYTES = 4;
    constexpr qsizetype CHUNK_HEADER_BYTES = sizeof(uint32) + CHUNK_TYPE_BYTES;
    constexpr qsizetype CHUNK_OVERHEAD_BYTES = CHUNK_HEADER_BYTES + sizeof(uint32);
    constexpr qsizetype IHDR_FIELDS_BYTES = 2 * sizeof(uint32) + 5; // ludus-style: magic-number PNG's five byte fields
    constexpr qsizetype BIT_DEPTH_OFFSET = 2 * sizeof(uint32);
    constexpr qsizetype COLOR_TYPE_OFFSET = BIT_DEPTH_OFFSET + 1;
    constexpr uint8 GRAYSCALE_COLOR_TYPE = 0;
    constexpr uint8 GRAYSCALE8_BITS = 8;
    constexpr uint8 GRAYSCALE16_BITS = 16;
    QByteArray png = signature;
    qsizetype offset = signature.size();
    bool hasHeader = false;
    bool hasImageData = false;
    while (bytes.size() - offset >= CHUNK_OVERHEAD_BYTES)
    {
        const uint8* chunk = reinterpret_cast<const uint8*>(bytes.constData() + offset);
        uint32 length = 0;
        static_cast<void>(TryReadBigEndian({chunk, sizeof(uint32)}, length));
        if (static_cast<qsizetype>(length) > bytes.size() - offset - CHUNK_OVERHEAD_BYTES)
        {
            break;
        }
        const QByteArrayView type(bytes.constData() + offset + sizeof(uint32), CHUNK_TYPE_BYTES);
        const qsizetype chunkBytes = CHUNK_OVERHEAD_BYTES + static_cast<qsizetype>(length);
        const uint8* data = chunk + CHUNK_HEADER_BYTES;
        if (hasHeader == false)
        {
            uint32 width = 0;
            uint32 height = 0;
            if (type != QByteArrayView("IHDR") || length != IHDR_FIELDS_BYTES ||
                TryReadBigEndian({data, sizeof(uint32)}, width) == false ||
                TryReadBigEndian({data + sizeof(uint32), sizeof(uint32)}, height) == false)
            {
                break;
            }
            if (width < 2 || height < 2 || width > TERRAIN_HEIGHTMAP_MAX_SIDE || height > TERRAIN_HEIGHTMAP_MAX_SIDE)
            {
                return RefuseTerrainSource(outError,
                                           QStringLiteral("Choose a grayscale PNG with 2–%1 pixels per axis.")
                                               .arg(TERRAIN_HEIGHTMAP_MAX_SIDE));
            }
            if (data[COLOR_TYPE_OFFSET] != GRAYSCALE_COLOR_TYPE ||
                (data[BIT_DEPTH_OFFSET] != GRAYSCALE8_BITS && data[BIT_DEPTH_OFFSET] != GRAYSCALE16_BITS))
            {
                return RefuseTerrainSource(outError,
                                           QStringLiteral("Export an 8- or 16-bit grayscale PNG without alpha."));
            }
            hasHeader = true;
            png.append(bytes.constData() + offset, chunkBytes);
        }
        else if (type == QByteArrayView("IDAT"))
        {
            hasImageData = true;
            png.append(bytes.constData() + offset, chunkBytes);
        }
        else if (type == QByteArrayView("IEND") && hasImageData == true && length == 0 &&
                 offset + chunkBytes == bytes.size())
        {
            png.append(bytes.constData() + offset, chunkBytes);
            outPng = Move(png);
            return true;
        }
        else if (type == QByteArrayView("tRNS") || type == QByteArrayView("acTL") || type == QByteArrayView("fcTL") ||
                 type == QByteArrayView("fdAT") || type[0] < 'a' || type[0] > 'z')
        {
            return RefuseTerrainSource(outError, QStringLiteral("Export a static grayscale PNG without transparency."));
        }
        offset += chunkBytes;
    }
    return RefuseTerrainSource(outError, QStringLiteral("Invalid PNG chunk layout. Re-export the heightmap."));
}

bool IsSha256(const QString& digest)
{
    if (digest.size() != SHA256_HEX_LENGTH)
    {
        return false;
    }
    for (const QChar digit : digest)
    {
        if ((digit < QLatin1Char('0') || digit > QLatin1Char('9')) &&
            (digit < QLatin1Char('a') || digit > QLatin1Char('f')))
        {
            return false;
        }
    }
    return true;
}

QJsonArray GetStrokes(const QJsonObject& recipe)
{
    return recipe.value(QStringLiteral("sculpt")).toObject().value(QStringLiteral("strokes")).toArray();
}

void SetStrokes(const QJsonArray& strokes, QJsonObject& inoutRecipe)
{
    if (strokes.isEmpty() == true)
    {
        inoutRecipe.remove(QStringLiteral("sculpt"));
        return;
    }
    inoutRecipe.insert(
        QStringLiteral("sculpt"),
        QJsonObject{{QStringLiteral("version"), TERRAIN_STROKE_VERSION}, {QStringLiteral("strokes"), strokes}});
}

float64 ReadGraySample(const QImage& image, int32 x, int32 z) noexcept
{
    const uint8* row = image.constScanLine(z);
    if (image.format() == QImage::Format_Grayscale16)
    {
        constexpr float64 GRAYSCALE16_WHITE = 65535;
        return static_cast<float64>(reinterpret_cast<const uint16*>(row)[x]) / GRAYSCALE16_WHITE;
    }
    constexpr float64 GRAYSCALE8_WHITE = 255;
    return static_cast<float64>(row[x]) / GRAYSCALE8_WHITE;
}

float64 ComputeSmoothedHeight(const QVector<math::Vector3>& before, uint32 samples, uint32 x, uint32 z)
{
    const uint32 firstX = x == 0 ? 0 : x - 1;
    const uint32 firstZ = z == 0 ? 0 : z - 1;
    const uint32 lastX = math::Min(x + 1, samples - 1);
    const uint32 lastZ = math::Min(z + 1, samples - 1);
    float64 sum = 0;
    uint32 count = 0;
    for (uint32 row = firstZ; row <= lastZ; ++row)
    {
        for (uint32 column = firstX; column <= lastX; ++column)
        {
            sum += static_cast<float64>(before[row * samples + column].Y);
            ++count;
        }
    }
    return sum / count;
}

bool IsAxisCoverageClipped(float64 coordinate, float64 radius, float64 previousExtent, float64 replacementExtent)
{
    const float64 previousHalfExtent = previousExtent * 0.5;
    const float64 replacementHalfExtent = replacementExtent * 0.5;
    return math::Max(coordinate - radius, -previousHalfExtent) < -replacementHalfExtent ||
           math::Min(coordinate + radius, previousHalfExtent) > replacementHalfExtent;
}
} // namespace

float64 TerrainHeightmap::Sample(float64 u, float64 v) const noexcept
{
    const float64 x = u * (Image.width() - 1);
    const float64 z = v * (Image.height() - 1);
    const int32 left = static_cast<int32>(x);
    const int32 top = static_cast<int32>(z);
    const int32 right = math::Min(left + 1, Image.width() - 1);
    const int32 bottom = math::Min(top + 1, Image.height() - 1);
    const float64 first = math::Lerp(ReadGraySample(Image, left, top), ReadGraySample(Image, right, top), x - left);
    const float64 second =
        math::Lerp(ReadGraySample(Image, left, bottom), ReadGraySample(Image, right, bottom), x - left);
    return math::Lerp(first, second, z - top);
}

bool LoadTerrainHeightmap(const QString& root,
                          const QString& relative,
                          const QString& expectedDigest,
                          TerrainHeightmap& outHeightmap,
                          QString& outError)
{
    const QString path = QDir(root).absoluteFilePath(relative);
    if (IsValidHeightmapPath(relative) == false || ProjectFiles::IsProjectFile(root, path) == false)
    {
        return RefuseTerrainSource(
            outError,
            QStringLiteral("Choose a regular PNG inside the project. Copy the heightmap into the project first."));
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false || file.size() <= 0 || file.size() > TERRAIN_HEIGHTMAP_MAX_FILE_BYTES)
    {
        return RefuseTerrainSource(
            outError,
            QStringLiteral("Choose a readable PNG no larger than %1 bytes.").arg(TERRAIN_HEIGHTMAP_MAX_FILE_BYTES));
    }
    QByteArray bytes = file.read(TERRAIN_HEIGHTMAP_MAX_FILE_BYTES + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != file.size() ||
        bytes.size() > TERRAIN_HEIGHTMAP_MAX_FILE_BYTES)
    {
        return RefuseTerrainSource(outError, QStringLiteral("The heightmap changed while reading. Choose it again."));
    }
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (expectedDigest.isEmpty() == false && digest != expectedDigest)
    {
        return RefuseTerrainSource(outError,
                                   QStringLiteral("The saved heightmap bytes changed. Restore the original or choose "
                                                  "the file again to preview the new source."));
    }
    QByteArray png;
    if (ExtractHeightmapPng(bytes, png, outError) == false)
    {
        return false;
    }
    QBuffer buffer(&png);
    static_cast<void>(buffer.open(QIODevice::ReadOnly));
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    if (size.width() < 2 || size.height() < 2 || size.width() > TERRAIN_HEIGHTMAP_MAX_SIDE ||
        size.height() > TERRAIN_HEIGHTMAP_MAX_SIDE)
    {
        return RefuseTerrainSource(
            outError,
            QStringLiteral("Choose a grayscale PNG with 2–%1 pixels per axis.").arg(TERRAIN_HEIGHTMAP_MAX_SIDE));
    }
    const QImage::Format format = reader.imageFormat();
    if (format != QImage::Format_Grayscale8 && format != QImage::Format_Grayscale16)
    {
        return RefuseTerrainSource(outError, QStringLiteral("Export an 8- or 16-bit grayscale PNG without alpha."));
    }
    QImage image = reader.read();
    if (image.isNull() == true || image.size() != size || image.format() != format)
    {
        return RefuseTerrainSource(outError, QStringLiteral("The PNG could not be decoded. Re-export the heightmap."));
    }
    outHeightmap = {Move(image), digest};
    outError.clear();
    return true;
}

QJsonObject GetTerrainSchemaFields(QJsonObject recipe)
{
    recipe.remove(QStringLiteral("heightmap"));
    recipe.remove(QStringLiteral("sculpt"));
    return recipe;
}

bool ValidateTerrainSource(const QJsonObject& recipe, QString& outError)
{
    const QJsonValue source = recipe.value(QStringLiteral("heightmap"));
    if (source.isUndefined() == false)
    {
        const QJsonObject heightmap = source.toObject();
        if (heightmap.size() != HEIGHTMAP_FIELDS ||
            IsValidHeightmapPath(heightmap.value(QStringLiteral("path")).toString()) == false ||
            IsSha256(heightmap.value(QStringLiteral("sha256")).toString()) == false ||
            heightmap.value(QStringLiteral("flip_x")).isBool() == false ||
            heightmap.value(QStringLiteral("flip_z")).isBool() == false)
        {
            return RefuseTerrainSource(outError,
                                       QStringLiteral("Invalid heightmap reference. Choose the source again."));
        }
    }
    const QJsonValue layerValue = recipe.value(QStringLiteral("sculpt"));
    if (layerValue.isUndefined() == true)
    {
        return true;
    }
    const QJsonObject layer = layerValue.toObject();
    const QJsonValue strokesValue = layer.value(QStringLiteral("strokes"));
    if (layer.size() != 2 || layer.value(QStringLiteral("version")) != TERRAIN_STROKE_VERSION ||
        strokesValue.isArray() == false)
    {
        return RefuseTerrainSource(outError,
                                   QStringLiteral("Unsupported terrain sculpt data. Open a compatible World."));
    }
    const QJsonArray strokes = strokesValue.toArray();
    const float64 halfWidth = recipe.value(QStringLiteral("width")).toDouble() * 0.5;
    const float64 halfDepth = recipe.value(QStringLiteral("depth")).toDouble() * 0.5;
    qsizetype pointCount = 0;
    if (strokes.size() > TERRAIN_LAYER_MAX_POINTS)
    {
        return RefuseTerrainSource(outError, QStringLiteral("The terrain's sculpt stroke budget is exhausted."));
    }
    for (const QJsonValue value : strokes)
    {
        const QJsonObject stroke = value.toObject();
        const QJsonArray points = stroke.value(QStringLiteral("points")).toArray();
        TerrainBrushKind kind;
        if (stroke.size() != STROKE_FIELDS ||
            TryReadBrushKind(stroke.value(QStringLiteral("kind")).toString(), kind) == false ||
            IsInTerrainRange(stroke.value(QStringLiteral("radius")),
                             TERRAIN_BRUSH_MIN_RADIUS_METRES,
                             TERRAIN_BRUSH_MAX_RADIUS_METRES) == false ||
            IsInTerrainRange(stroke.value(QStringLiteral("strength")), 0, 1) == false ||
            IsInTerrainRange(stroke.value(QStringLiteral("target")),
                             -TERRAIN_BRUSH_ELEVATION_LIMIT_METRES,
                             TERRAIN_BRUSH_ELEVATION_LIMIT_METRES) == false ||
            points.isEmpty() == true || points.size() > TERRAIN_STROKE_MAX_POINTS)
        {
            return RefuseTerrainSource(
                outError,
                QStringLiteral("Invalid sculpt stroke. Check radius, strength and target elevation."));
        }
        pointCount += points.size();
        if (pointCount > TERRAIN_LAYER_MAX_POINTS)
        {
            return RefuseTerrainSource(outError,
                                       QStringLiteral("Terrain has reached its %1-dab budget. Undo a stroke or use "
                                                      "a shorter stroke.")
                                           .arg(TERRAIN_LAYER_MAX_POINTS));
        }
        for (const QJsonValue pointValue : points)
        {
            const QJsonArray point = pointValue.toArray();
            if (point.size() != 2 || IsInTerrainRange(point[0], -halfWidth, halfWidth) == false ||
                IsInTerrainRange(point[1], -halfDepth, halfDepth) == false)
            {
                return RefuseTerrainSource(outError,
                                           QStringLiteral("A sculpt stroke lies outside the terrain footprint."));
            }
        }
    }
    return true;
}

bool AppendTerrainStroke(const TerrainBrush& brush,
                         const QJsonArray& points,
                         QJsonObject& inoutRecipe,
                         QString& outError)
{
    QJsonArray strokes = GetStrokes(inoutRecipe);
    strokes.append(QJsonObject{{QStringLiteral("kind"), GetBrushName(brush.Kind)},
                               {QStringLiteral("radius"), brush.RadiusMetres},
                               {QStringLiteral("strength"), brush.Strength},
                               {QStringLiteral("target"), brush.TargetElevationMetres},
                               {QStringLiteral("points"), points}});
    QJsonObject candidate = inoutRecipe;
    SetStrokes(strokes, candidate);
    if (ValidateTerrainSource(candidate, outError) == false)
    {
        return false;
    }
    inoutRecipe = Move(candidate);
    return true;
}

bool ApplyTerrainStrokes(const QJsonObject& recipe,
                         uint32 samples,
                         QVector<math::Vector3>& inoutPositionsMesh,
                         QString& outError)
{
    for (const QJsonValue value : GetStrokes(recipe))
    {
        const QJsonObject stroke = value.toObject();
        TerrainBrushKind kind;
        if (TryReadBrushKind(stroke.value(QStringLiteral("kind")).toString(), kind) == false)
        {
            return RefuseTerrainSource(outError, QStringLiteral("Unknown sculpt brush. Open a compatible World."));
        }
        const float64 radius = stroke.value(QStringLiteral("radius")).toDouble();
        const float64 strength = stroke.value(QStringLiteral("strength")).toDouble();
        const float64 flatten = stroke.value(QStringLiteral("target")).toDouble();
        const QVector<math::Vector3> before =
            kind == TerrainBrushKind::Smooth ? inoutPositionsMesh : QVector<math::Vector3>{};
        for (const QJsonValue pointValue : stroke.value(QStringLiteral("points")).toArray())
        {
            const QJsonArray point = pointValue.toArray();
            for (uint32 z = 0; z < samples; ++z)
            {
                for (uint32 x = 0; x < samples; ++x)
                {
                    math::Vector3& positionMesh = inoutPositionsMesh[z * samples + x];
                    const float64 deltaX = static_cast<float64>(positionMesh.X) - point[0].toDouble();
                    const float64 deltaZ = static_cast<float64>(positionMesh.Z) - point[1].toDouble();
                    const float64 squaredDistance = deltaX * deltaX + deltaZ * deltaZ;
                    if (squaredDistance >= radius * radius)
                    {
                        continue;
                    }
                    const float64 weight = strength * math::SmoothStep01(1 - math::Sqrt(squaredDistance) / radius);
                    const float64 elevation = static_cast<float64>(positionMesh.Y);
                    float64 target = flatten;
                    switch (kind)
                    {
                        case TerrainBrushKind::Raise:
                            target = elevation + RAISE_METRES_PER_DAB;
                            break;
                        case TerrainBrushKind::Lower:
                            target = elevation - RAISE_METRES_PER_DAB;
                            break;
                        case TerrainBrushKind::Smooth:
                            target = ComputeSmoothedHeight(before, samples, x, z);
                            break;
                        case TerrainBrushKind::Flatten:
                            break;
                    }
                    const float64 result = math::Lerp(elevation, target, weight);
                    if (math::Abs(result) > TERRAIN_BRUSH_ELEVATION_LIMIT_METRES)
                    {
                        return RefuseTerrainSource(outError,
                                                   QStringLiteral("The stroke exceeds the terrain elevation "
                                                                  "limit. Reduce brush strength or relief."));
                    }
                    positionMesh.Y = static_cast<float32>(result);
                }
            }
        }
    }
    return true;
}

QJsonArray FindClippedTerrainStrokes(const QJsonObject& previous, const QJsonObject& replacement)
{
    const QJsonArray strokes = GetStrokes(previous);
    QJsonArray clipped;
    const float64 previousWidth = previous.value(QStringLiteral("width")).toDouble();
    const float64 previousDepth = previous.value(QStringLiteral("depth")).toDouble();
    const float64 width = replacement.value(QStringLiteral("width")).toDouble();
    const float64 depth = replacement.value(QStringLiteral("depth")).toDouble();
    for (int32 index = 0; index < strokes.size(); ++index)
    {
        const QJsonObject stroke = strokes[index].toObject();
        const float64 radius = stroke.value(QStringLiteral("radius")).toDouble();
        for (const QJsonValue pointValue : stroke.value(QStringLiteral("points")).toArray())
        {
            const QJsonArray point = pointValue.toArray();
            if (IsAxisCoverageClipped(point[0].toDouble(), radius, previousWidth, width) == true ||
                IsAxisCoverageClipped(point[1].toDouble(), radius, previousDepth, depth) == true)
            {
                clipped.append(index);
                break;
            }
        }
    }
    return clipped;
}

void RemoveTerrainStrokes(const QJsonArray& indices, QJsonObject& inoutRecipe)
{
    QJsonArray strokes = GetStrokes(inoutRecipe);
    for (qsizetype index = indices.size(); index > 0; --index)
    {
        strokes.removeAt(indices[index - 1].toInt());
    }
    SetStrokes(strokes, inoutRecipe);
}
} // namespace ludus::editor
