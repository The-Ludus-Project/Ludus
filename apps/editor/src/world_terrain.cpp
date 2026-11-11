#include "internal/world_terrain.h"

#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QList>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;

constexpr int32 PROCEDURAL_TERRAIN_VERSION = 1;
constexpr int32 AUTHORED_TERRAIN_VERSION = 2;
constexpr int32 SCULPT_TERRAIN_VERSION = 3;
constexpr int32 MAX_TERRAIN_SAMPLES_PER_AXIS = 64;
constexpr qsizetype MAX_TERRAIN_STROKES = 16;
constexpr int64 MAX_TERRAIN_HEIGHTMAP_FILE_BYTES = 64 * 1024;
constexpr uint32 HEIGHTMAP_SAMPLE_MAXIMUM = 65535;
constexpr uint32 GRAYSCALE_BYTE_MAXIMUM = 255;
constexpr qsizetype PNG_BIT_DEPTH_OFFSET = 24;
constexpr qsizetype PNG_COLOUR_TYPE_OFFSET = PNG_BIT_DEPTH_OFFSET + 1;
constexpr char PNG_GRAYSCALE_COLOUR_TYPE = 0;
constexpr char PNG_EIGHT_BIT_DEPTH = 8;
constexpr char PNG_SIXTEEN_BIT_DEPTH = 16;
constexpr qsizetype HEIGHTMAP_FIELDS = 3;
constexpr qsizetype FLATTEN_STROKE_FIELDS = 3;
constexpr qsizetype SCULPT_STROKE_FIELDS = FLATTEN_STROKE_FIELDS + 1;
constexpr QLatin1StringView HEIGHTMAP_FIELD("heightmap");
constexpr QLatin1StringView FLATTEN_STROKES_FIELD("flattenStrokes");
constexpr QLatin1StringView SCULPT_STROKES_FIELD("sculptStrokes");
constexpr QLatin1StringView RAISE_LOWER_KIND("RaiseLower");
constexpr QLatin1StringView SMOOTH_KIND("Smooth");
constexpr QLatin1StringView HEIGHTMAP_SHAPE("Heightmap");
constexpr uint32 TERRAIN_RANDOM_DOMAIN = 1;
constexpr uint32 INDICES_PER_CELL = 6;
struct TerrainStroke final
{
    QList<math::Vector2> PointsMesh;
    float32 Radius = 0;
    TerrainBrush Brush = TerrainBrush::Flatten;
    float32 Value = 0;
};

struct TerrainPayload final
{
    QList<uint16> Heights;
    QList<TerrainStroke> Strokes;
    uint32 Columns = 0;
    uint32 Rows = 0;
    bool ShouldFlipX = false;
    bool ShouldFlipZ = false;
};

bool IsGridAxisAdmitted(qsizetype count)
{
    return count >= 2 && count <= MAX_TERRAIN_SAMPLES_PER_AXIS;
}

bool ReadHeightmap(const QJsonValue& value, TerrainPayload& inoutPayload)
{
    const QJsonObject map = value.toObject();
    const QJsonValue flipX = map.value(QStringLiteral("flipX"));
    const QJsonValue flipZ = map.value(QStringLiteral("flipZ"));
    const QJsonArray rows = map.value(QStringLiteral("rows")).toArray();
    if (map.size() != HEIGHTMAP_FIELDS || flipX.isBool() == false || flipZ.isBool() == false ||
        IsGridAxisAdmitted(rows.size()) == false)
    {
        return false;
    }
    const qsizetype columns = rows.first().toArray().size();
    if (IsGridAxisAdmitted(columns) == false)
    {
        return false;
    }
    inoutPayload.Heights.reserve(rows.size() * columns);
    for (const QJsonValue row : rows)
    {
        const QJsonArray samples = row.toArray();
        if (samples.size() != columns)
        {
            return false;
        }
        for (const QJsonValue sample : samples)
        {
            const int32 code = sample.toInt(-1);
            if (code < 0 || static_cast<uint32>(code) > HEIGHTMAP_SAMPLE_MAXIMUM)
            {
                return false;
            }
            inoutPayload.Heights.append(static_cast<uint16>(code));
        }
    }
    inoutPayload.Columns = static_cast<uint32>(columns);
    inoutPayload.Rows = static_cast<uint32>(rows.size());
    inoutPayload.ShouldFlipX = flipX.toBool();
    inoutPayload.ShouldFlipZ = flipZ.toBool();
    return true;
}

QLatin1StringView GetTerrainStrokeField(const QJsonObject& recipe)
{
    return recipe.contains(SCULPT_STROKES_FIELD) == true ? SCULPT_STROKES_FIELD : FLATTEN_STROKES_FIELD;
}

int32 GetTerrainRecipeVersion(const QJsonObject& recipe)
{
    if (recipe.contains(SCULPT_STROKES_FIELD) == true)
    {
        return SCULPT_TERRAIN_VERSION;
    }
    return recipe.contains(HEIGHTMAP_FIELD) == true || recipe.contains(FLATTEN_STROKES_FIELD) == true
               ? AUTHORED_TERRAIN_VERSION
               : PROCEDURAL_TERRAIN_VERSION;
}

bool ReadTerrainStroke(const QJsonValue& value,
                       math::Vector2 halfExtentMesh,
                       bool canUseSculptKinds,
                       TerrainStroke& outStroke)
{
    const QJsonObject stroke = value.toObject();
    const float64 radius = stroke.value(QStringLiteral("radius")).toDouble();
    const QJsonValue kind = stroke.value(QStringLiteral("kind"));
    const QJsonArray points = stroke.value(QStringLiteral("points")).toArray();
    if (radius < MIN_TERRAIN_BRUSH_RADIUS_METRES || radius > MAX_TERRAIN_BRUSH_RADIUS_METRES ||
        math::IsFinite(radius) == false || points.isEmpty() == true || points.size() > MAX_TERRAIN_STROKE_POINTS)
    {
        return false;
    }
    outStroke.Radius = static_cast<float32>(radius);
    const bool isFlatten = kind.isUndefined();
    const QJsonValue amount = stroke.value(isFlatten == true ? QStringLiteral("height") : QStringLiteral("amount"));
    const qsizetype fieldCount = isFlatten == true ? FLATTEN_STROKE_FIELDS : SCULPT_STROKE_FIELDS;
    if (stroke.size() != fieldCount || amount.isDouble() == false || math::IsFinite(amount.toDouble()) == false)
    {
        return false;
    }
    const float64 strokeValue = amount.toDouble();
    if (isFlatten == true && math::Abs(strokeValue) <= MAX_TERRAIN_BRUSH_HEIGHT_METRES)
    {
        outStroke.Brush = TerrainBrush::Flatten;
    }
    else if (canUseSculptKinds == true && kind.toString() == RAISE_LOWER_KIND && strokeValue != 0 &&
             math::Abs(strokeValue) <= MAX_TERRAIN_BRUSH_HEIGHT_METRES)
    {
        outStroke.Brush = TerrainBrush::RaiseLower;
    }
    else if (canUseSculptKinds == true && kind.toString() == SMOOTH_KIND && strokeValue > 0 && strokeValue <= 1)
    {
        outStroke.Brush = TerrainBrush::Smooth;
    }
    else
    {
        return false;
    }
    outStroke.Value = static_cast<float32>(strokeValue);
    outStroke.PointsMesh.reserve(points.size());
    for (const QJsonValue point : points)
    {
        const QJsonArray pair = point.toArray();
        if (pair.size() != 2 || pair[0].isDouble() == false || pair[1].isDouble() == false)
        {
            return false;
        }
        const float64 x = pair[0].toDouble();
        const float64 z = pair[1].toDouble();
        if (math::IsFinite(x) == false || math::IsFinite(z) == false ||
            math::Abs(x) > static_cast<float64>(halfExtentMesh.X) ||
            math::Abs(z) > static_cast<float64>(halfExtentMesh.Y))
        {
            return false;
        }
        outStroke.PointsMesh.append({static_cast<float32>(x), static_cast<float32>(z)});
    }
    return true;
}

bool ReadTerrainPayload(const QJsonObject& recipe, TerrainPayload& inoutPayload, QString& outError)
{
    const bool hasHeightmap = recipe.contains(HEIGHTMAP_FIELD);
    const bool hasSculptStrokes = recipe.contains(SCULPT_STROKES_FIELD);
    const QLatin1StringView strokesField = GetTerrainStrokeField(recipe);
    if (recipe.value(QStringLiteral("version")).toInt() != GetTerrainRecipeVersion(recipe) ||
        (hasSculptStrokes == true && recipe.contains(FLATTEN_STROKES_FIELD) == true))
    {
        outError = QStringLiteral("This terrain recipe version is unavailable. Open it in a compatible editor.");
        return false;
    }
    const bool hasHeightmapSource = recipe.value(QStringLiteral("shape")).toString() == HEIGHTMAP_SHAPE;
    if (hasHeightmap != hasHeightmapSource ||
        (hasHeightmap == true && ReadHeightmap(recipe.value(HEIGHTMAP_FIELD), inoutPayload) == false))
    {
        outError = QStringLiteral("Choose a valid grayscale heightmap with 2–%1 samples on each axis.")
                       .arg(MAX_TERRAIN_SAMPLES_PER_AXIS);
        return false;
    }
    if (recipe.contains(strokesField) == true)
    {
        const QJsonArray strokes = recipe.value(strokesField).toArray();
        if (strokes.isEmpty() == true || strokes.size() > MAX_TERRAIN_STROKES)
        {
            outError = QStringLiteral("A terrain supports up to %1 sculpt strokes. Undo a stroke to free space.")
                           .arg(MAX_TERRAIN_STROKES);
            return false;
        }
        const math::Vector2 halfExtentMesh{
            static_cast<float32>(recipe.value(QStringLiteral("width")).toDouble() * 0.5),
            static_cast<float32>(recipe.value(QStringLiteral("depth")).toDouble() * 0.5)};
        for (const QJsonValue value : strokes)
        {
            TerrainStroke stroke;
            if (ReadTerrainStroke(value, halfExtentMesh, hasSculptStrokes, stroke) == false)
            {
                outError = QStringLiteral(
                    "Keep stroke centres inside the terrain and choose a valid radius, height, or strength.");
                return false;
            }
            inoutPayload.Strokes.append(Move(stroke));
        }
    }
    return true;
}

float32 ComputeTerrainStrokeWeight(const TerrainStroke& stroke, math::Vector2 positionMesh)
{
    const float32 radiusSquared = stroke.Radius * stroke.Radius;
    float32 distanceSquared = math::LengthSquared(positionMesh - stroke.PointsMesh.first());
    for (qsizetype index = 1; index < stroke.PointsMesh.size(); ++index)
    {
        const math::Vector2 startMesh = stroke.PointsMesh[index - 1];
        const math::Vector2 directionMesh = stroke.PointsMesh[index] - startMesh;
        const float32 lengthSquared = math::LengthSquared(directionMesh);
        if (lengthSquared > 0)
        {
            const float32 along =
                math::Clamp(math::Dot(positionMesh - startMesh, directionMesh) / lengthSquared, 0.0F, 1.0F);
            distanceSquared =
                math::Min(distanceSquared, math::LengthSquared(positionMesh - (startMesh + directionMesh * along)));
        }
    }
    if (distanceSquared >= radiusSquared)
    {
        return 0;
    }
    const float32 distance = math::Sqrt(distanceSquared) / stroke.Radius;
    return 1 - math::SmoothStep01(math::Clamp(2 * distance - 1, 0.0F, 1.0F));
}

void ApplyTerrainStrokes(const QList<TerrainStroke>& strokes,
                         uint32 columns,
                         uint32 rows,
                         QList<math::Vector3>& inoutPositions)
{
    QList<float32> previousHeights;
    for (const TerrainStroke& stroke : strokes)
    {
        if (stroke.Brush == TerrainBrush::Smooth)
        {
            // Read the same pre-stroke surface at every vertex so traversal order cannot affect smoothing.
            previousHeights.resize(inoutPositions.size());
            for (qsizetype index = 0; index < inoutPositions.size(); ++index)
            {
                previousHeights[index] = inoutPositions[index].Y;
            }
        }
        for (uint32 z = 0; z < rows; ++z)
        {
            for (uint32 x = 0; x < columns; ++x)
            {
                const uint32 index = z * columns + x;
                math::Vector3& positionMesh = inoutPositions[index];
                const float32 weight = ComputeTerrainStrokeWeight(stroke, {positionMesh.X, positionMesh.Z});
                if (weight == 0)
                {
                    continue;
                }
                if (stroke.Brush == TerrainBrush::Flatten)
                {
                    positionMesh.Y = math::Lerp(positionMesh.Y, stroke.Value, weight);
                }
                else if (stroke.Brush == TerrainBrush::RaiseLower)
                {
                    positionMesh.Y += stroke.Value * weight;
                }
                else
                {
                    // Clamp the neighbour lookup at the mesh border.
                    const uint32 left = x == 0 ? index : index - 1;
                    const uint32 right = x + 1 == columns ? index : index + 1;
                    const uint32 front = z == 0 ? index : index - columns;
                    const uint32 back = z + 1 == rows ? index : index + columns;
                    constexpr float32 NEIGHBOUR_WEIGHT = 0.25F;
                    const float32 meanHeightMesh = (previousHeights[left] + previousHeights[right] +
                                                    previousHeights[front] + previousHeights[back]) *
                                                   NEIGHBOUR_WEIGHT;
                    positionMesh.Y = math::Lerp(positionMesh.Y, meanHeightMesh, weight * stroke.Value);
                }
            }
        }
    }
}

void BuildTerrainNormals(uint32 columns, uint32 rows, PreviewMesh& inoutMesh)
{
    for (uint32 z = 0; z < rows; ++z)
    {
        const uint32 previousZ = z == 0 ? z : z - 1;
        const uint32 nextZ = math::Min(z + 1, rows - 1);
        for (uint32 x = 0; x < columns; ++x)
        {
            const uint32 previousX = x == 0 ? x : x - 1;
            const uint32 nextX = math::Min(x + 1, columns - 1);
            const math::Vector3 tangentX =
                inoutMesh.Positions[z * columns + nextX] - inoutMesh.Positions[z * columns + previousX];
            const math::Vector3 tangentZ =
                inoutMesh.Positions[nextZ * columns + x] - inoutMesh.Positions[previousZ * columns + x];
            math::Vector3 normal{0, 1, 0};
            (void)math::TryNormalize(math::Cross(tangentZ, tangentX), normal);
            inoutMesh.Normals.append(normal);
        }
    }
}
} // namespace

QString GetTerrainComponentId()
{
    return QStringLiteral("terrain");
}

qsizetype CountTerrainPayloadFields(const QJsonObject& recipe)
{
    return static_cast<qsizetype>(recipe.contains(HEIGHTMAP_FIELD)) +
           static_cast<qsizetype>(recipe.contains(FLATTEN_STROKES_FIELD)) +
           static_cast<qsizetype>(recipe.contains(SCULPT_STROKES_FIELD));
}

void SetTerrainRecipeVersion(QJsonObject& inoutRecipe)
{
    inoutRecipe.insert(QStringLiteral("version"), GetTerrainRecipeVersion(inoutRecipe));
}

bool ImportTerrainHeightmap(const QString& path, QJsonObject& outHeightmap, QString& outError)
{
    const QFileInfo source(path);
    QFile file(path);
    if (source.isFile() == false || source.size() > MAX_TERRAIN_HEIGHTMAP_FILE_BYTES ||
        file.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Open a readable grayscale PNG no larger than %1 bytes.")
                       .arg(MAX_TERRAIN_HEIGHTMAP_FILE_BYTES);
        return false;
    }
    QByteArray bytes = file.read(MAX_TERRAIN_HEIGHTMAP_FILE_BYTES + 1);
    if (bytes.size() > MAX_TERRAIN_HEIGHTMAP_FILE_BYTES || file.error() != QFileDevice::NoError)
    {
        outError =
            QStringLiteral("The heightmap could not be read within the file limit. Export a smaller grayscale PNG.");
        return false;
    }
    // IHDR precedes image data. Check sample encoding before the decoder expands it.
    if (bytes.size() <= PNG_COLOUR_TYPE_OFFSET || bytes[PNG_COLOUR_TYPE_OFFSET] != PNG_GRAYSCALE_COLOUR_TYPE ||
        (bytes[PNG_BIT_DEPTH_OFFSET] != PNG_EIGHT_BIT_DEPTH && bytes[PNG_BIT_DEPTH_OFFSET] != PNG_SIXTEEN_BIT_DEPTH))
    {
        outError = QStringLiteral("Export the heightmap as an 8- or 16-bit grayscale PNG without an alpha channel.");
        return false;
    }
    QBuffer buffer(&bytes);
    if (buffer.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("The heightmap buffer could not be opened. Choose the PNG again.");
        return false;
    }
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    const QSize extent = reader.size();
    if (IsGridAxisAdmitted(extent.width()) == false || IsGridAxisAdmitted(extent.height()) == false)
    {
        outError =
            QStringLiteral("Export a grayscale PNG with 2–%1 pixels on each axis.").arg(MAX_TERRAIN_SAMPLES_PER_AXIS);
        return false;
    }
    const QImage image = reader.read();
    if (image.size() != extent ||
        (image.format() != QImage::Format_Grayscale8 && image.format() != QImage::Format_Grayscale16))
    {
        outError = QStringLiteral("Export the heightmap as an 8- or 16-bit grayscale PNG without an alpha channel.");
        return false;
    }
    QJsonArray rows;
    for (int32 z = 0; z < extent.height(); ++z)
    {
        QJsonArray row;
        const uint8* samples = image.constScanLine(z);
        for (int32 x = 0; x < extent.width(); ++x)
        {
            const uint32 code = image.format() == QImage::Format_Grayscale16
                                    ? reinterpret_cast<const uint16*>(samples)[x]
                                    : samples[x] * (HEIGHTMAP_SAMPLE_MAXIMUM / GRAYSCALE_BYTE_MAXIMUM);
            row.append(static_cast<int32>(code));
        }
        rows.append(row);
    }
    outHeightmap = {{QStringLiteral("rows"), rows}, {QStringLiteral("flipX"), false}, {QStringLiteral("flipZ"), false}};
    return true;
}

bool ValidateTerrainRecipe(const QJsonObject& recipe, QString& outError)
{
    if (recipe.value(QStringLiteral("minimumElevation")).toDouble() >
        recipe.value(QStringLiteral("maximumElevation")).toDouble())
    {
        outError = QStringLiteral("Set the maximum elevation at or above the minimum elevation.");
        return false;
    }
    TerrainPayload payload;
    return ReadTerrainPayload(recipe, payload, outError);
}

qsizetype RemoveClippedTerrainStrokes(const QJsonObject& previous, QJsonObject& inoutRecipe)
{
    const float64 halfWidth = inoutRecipe.value(QStringLiteral("width")).toDouble() * 0.5;
    const float64 halfDepth = inoutRecipe.value(QStringLiteral("depth")).toDouble() * 0.5;
    const bool isWidthShrinking =
        inoutRecipe.value(QStringLiteral("width")).toDouble() < previous.value(QStringLiteral("width")).toDouble();
    const bool isDepthShrinking =
        inoutRecipe.value(QStringLiteral("depth")).toDouble() < previous.value(QStringLiteral("depth")).toDouble();
    if (isWidthShrinking == false && isDepthShrinking == false)
    {
        return 0;
    }
    const QLatin1StringView strokesField = GetTerrainStrokeField(inoutRecipe);
    const QJsonArray strokes = inoutRecipe.value(strokesField).toArray();
    QJsonArray retained;
    for (const QJsonValue value : strokes)
    {
        const QJsonObject stroke = value.toObject();
        const float64 radius = stroke.value(QStringLiteral("radius")).toDouble();
        const QJsonArray points = stroke.value(QStringLiteral("points")).toArray();
        bool isClipped = false;
        for (const QJsonValue point : points)
        {
            const QJsonArray pair = point.toArray();
            if ((isWidthShrinking == true && math::Abs(pair[0].toDouble()) + radius > halfWidth) ||
                (isDepthShrinking == true && math::Abs(pair[1].toDouble()) + radius > halfDepth))
            {
                isClipped = true;
                break;
            }
        }
        if (isClipped == false)
        {
            retained.append(value);
        }
    }
    const qsizetype removed = strokes.size() - retained.size();
    if (removed != 0)
    {
        if (retained.isEmpty() == true)
        {
            inoutRecipe.remove(strokesField);
        }
        else
        {
            inoutRecipe.insert(strokesField, retained);
        }
        SetTerrainRecipeVersion(inoutRecipe);
    }
    return removed;
}

bool AppendTerrainStroke(QJsonObject& inoutRecipe,
                         const QJsonArray& points,
                         float32 radius,
                         TerrainBrush brush,
                         float32 value,
                         QString& outError)
{
    QJsonObject recipe = inoutRecipe;
    const QLatin1StringView previousField = GetTerrainStrokeField(recipe);
    QJsonArray strokes = recipe.value(previousField).toArray();
    QJsonObject stroke{{QStringLiteral("radius"), static_cast<float64>(radius)}, {QStringLiteral("points"), points}};
    QLatin1StringView strokesField = previousField;
    switch (brush)
    {
        case TerrainBrush::Off:
            outError = QStringLiteral("Choose a sculpt brush before starting a stroke.");
            return false;
        case TerrainBrush::Flatten:
            stroke.insert(QStringLiteral("height"), static_cast<float64>(value));
            break;
        case TerrainBrush::RaiseLower:
            if (value == 0)
            {
                outError = QStringLiteral("Enter a positive amount to raise terrain or a negative amount to lower it.");
                return false;
            }
            stroke.insert(QStringLiteral("kind"), QString(RAISE_LOWER_KIND));
            strokesField = SCULPT_STROKES_FIELD;
            break;
        case TerrainBrush::Smooth:
            stroke.insert(QStringLiteral("kind"), QString(SMOOTH_KIND));
            strokesField = SCULPT_STROKES_FIELD;
            break;
    }
    if (brush != TerrainBrush::Flatten)
    {
        stroke.insert(QStringLiteral("amount"), static_cast<float64>(value));
    }
    if (strokesField != previousField)
    {
        recipe.remove(previousField);
    }
    strokes.append(stroke);
    recipe.insert(strokesField, strokes);
    SetTerrainRecipeVersion(recipe);
    if (ValidateTerrainRecipe(recipe, outError) == false)
    {
        return false;
    }
    inoutRecipe = Move(recipe);
    return true;
}

bool BuildTerrainMesh(const QJsonObject& recipe, PreviewMesh& outMesh, QString& outError)
{
    TerrainPayload payload;
    if (ReadTerrainPayload(recipe, payload, outError) == false)
    {
        return false;
    }
    // The built-in World schema has already checked scalar field types and ranges.
    const uint32 samples = static_cast<uint32>(recipe.value(QStringLiteral("samples")).toInt());
    const float64 width = recipe.value(QStringLiteral("width")).toDouble();
    const float64 depth = recipe.value(QStringLiteral("depth")).toDouble();
    const QString preset = recipe.value(QStringLiteral("shape")).toString();
    const content::terrain::TerrainNoiseShape shape
    {
        .MinimumElevationMetres = recipe.value(QStringLiteral("minimumElevation")).toDouble(),
        .MaximumElevationMetres = recipe.value(QStringLiteral("maximumElevation")).toDouble(),
        .FeatureSizeMetres = recipe.value(QStringLiteral("featureSize")).toDouble(),
        .Roughness = recipe.value(QStringLiteral("roughness")).toDouble(),
        .Profile = preset == QStringLiteral("Flat")     ? content::terrain::TerrainNoiseProfile::Flat
                   : preset == QStringLiteral("Ridges") ? content::terrain::TerrainNoiseProfile::Ridges
                                                        : content::terrain::TerrainNoiseProfile::Hills,
    };
    if (IsGridAxisAdmitted(samples) == false)
    {
        outError = QStringLiteral("Choose Samples per side within the supported terrain mesh size.");
        return false;
    }
    if (width <= 0 || depth <= 0 ||
        content::terrain::CanSampleTerrainNoise(shape, math::Max(width, depth) * 0.5) == false)
    {
        outError = QStringLiteral("Use positive terrain dimensions and increase Feature size for this extent.");
        return false;
    }
    const bool hasHeightmapSource = payload.Heights.isEmpty() == false;
    const uint32 columns = hasHeightmapSource == true ? payload.Columns : samples;
    const uint32 rows = hasHeightmapSource == true ? payload.Rows : samples;
    const uint32 cellsX = columns - 1;
    const uint32 cellsZ = rows - 1;
    math::RandomKey key;
    if (math::TryMakeRandomKey(static_cast<uint64>(recipe.value(QStringLiteral("seed")).toInt()),
                               TERRAIN_RANDOM_DOMAIN,
                               key) != math::MathStatus::Success)
    {
        outError = QStringLiteral("Choose a valid terrain seed.");
        return false;
    }

    PreviewMesh mesh;
    mesh.Id = outMesh.Id;
    mesh.Positions.reserve(columns * rows);
    mesh.Uvs.reserve(columns * rows);
    mesh.Normals.reserve(columns * rows);
    mesh.Indices.reserve(INDICES_PER_CELL * cellsX * cellsZ);
    for (uint32 z = 0; z < rows; ++z)
    {
        const float64 v = static_cast<float64>(z) / cellsZ;
        const float64 positionZ = (v - 0.5) * depth;
        const uint32 sourceZ = payload.ShouldFlipZ == true ? cellsZ - z : z;
        for (uint32 x = 0; x < columns; ++x)
        {
            const float64 u = static_cast<float64>(x) / cellsX;
            const float64 positionX = (u - 0.5) * width;
            const uint32 sourceX = payload.ShouldFlipX == true ? cellsX - x : x;
            const float64 elevation =
                hasHeightmapSource == true
                    ? math::Lerp(shape.MinimumElevationMetres,
                                 shape.MaximumElevationMetres,
                                 static_cast<float64>(payload.Heights[sourceZ * columns + sourceX]) /
                                     HEIGHTMAP_SAMPLE_MAXIMUM)
                    : content::terrain::ComputeTerrainElevation(shape, key, positionX, positionZ);
            const math::Vector2 positionMesh{static_cast<float32>(positionX), static_cast<float32>(positionZ)};
            mesh.Positions.append({positionMesh.X, static_cast<float32>(elevation), positionMesh.Y});
            mesh.Uvs.append({static_cast<float32>(u), static_cast<float32>(v)});
            if (x < cellsX && z < cellsZ)
            {
                const uint32 corner = z * columns + x;
                const uint32 nextRow = corner + columns;
                mesh.Indices.append(corner);
                mesh.Indices.append(nextRow);
                mesh.Indices.append(corner + 1);
                mesh.Indices.append(corner + 1);
                mesh.Indices.append(nextRow);
                mesh.Indices.append(nextRow + 1);
            }
        }
    }
    ApplyTerrainStrokes(payload.Strokes, columns, rows, mesh.Positions);
    BuildTerrainNormals(columns, rows, mesh);
    outMesh = Move(mesh);
    return true;
}

bool EditWorldTerrain(WorldDocument& inoutWorld,
                      const identity::EntropySource& entropy,
                      const QString& key,
                      const QJsonObject& recipe,
                      QString& outKey,
                      QString& outError,
                      const QJsonObject& draftTransform)
{
    const QString component = GetTerrainComponentId();
    if (inoutWorld.HasBuiltInTerrain() == false)
    {
        outError = QStringLiteral("Open a default World to generate terrain.");
        return false;
    }
    if (ValidateTerrainRecipe(recipe, outError) == false)
    {
        return false;
    }
    if (key.isEmpty() == true)
    {
        QJsonObject transform;
        const QJsonObject definition = inoutWorld.Component(QStringLiteral("transform"));
        for (const QJsonValue value : definition.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject property = value.toObject();
            const QString id = property.value(QStringLiteral("id")).toString();
            transform.insert(id,
                             draftTransform.contains(id) == true ? draftTransform.value(id)
                                                                 : property.value(QStringLiteral("default")));
        }
        return inoutWorld.CreateEntity(entropy,
                                       outError,
                                       QStringLiteral("Terrain"),
                                       outKey,
                                       {{QStringLiteral("transform"), transform}, {component, recipe}},
                                       "Add Terrain");
    }
    const QJsonObject entity = inoutWorld.Entity(key);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    if (components.contains(component) == false)
    {
        outError = QStringLiteral("Select a terrain entity before editing its generation settings.");
        return false;
    }
    components.insert(component, recipe);
    if (inoutWorld.Edit(key, components, entity.value(QStringLiteral("name")).toString(), outError) == false)
    {
        return false;
    }
    outKey = key;
    return true;
}
} // namespace ludus::editor
