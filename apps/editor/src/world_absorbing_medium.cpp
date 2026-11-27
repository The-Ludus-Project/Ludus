#include "internal/world_absorbing_medium.h"

#include "internal/inspector_fields.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/content/scene_format/material.h>
#include <ludus/graphics/renderer/reference.hpp>

#include <QJsonValue>
#include <QStringList>

// Thanks to Julian Fong, Magnus Wrenninge, Christopher Kulla and Ralf Habel, "Production Volume Rendering",
// SIGGRAPH 2017 course, https://doi.org/10.1145/3084873.3084907: section 7.5 describes Hyperion's
// extinction-distance control, which lets an artist compare a medium's attenuation scale with the size of a
// volume. Properties shows the analogous absorption distance 1/sigma_a for each channel.
namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;
namespace format = content::scene_format;
namespace renderer = graphics::renderer;

/// Corners of the box, indexed by three sign bits: bit 0 is +X, bit 1 is +Y and bit 2 is +Z.
constexpr usize BOX_CORNERS = 8;
constexpr usize X_CORNER_BIT = 1;
constexpr usize Y_CORNER_BIT = 2;
constexpr usize Z_CORNER_BIT = 4;
constexpr usize BOX_FACES = 6;
constexpr usize QUAD_CORNERS = 4;
/// Each face's corners, counterclockwise seen from outside: cross(c1 - c0, c3 - c0) points out of the box.
constexpr StaticArray<StaticArray<usize, QUAD_CORNERS>, BOX_FACES> BOX_FACE_CORNERS{{
    {{0, 2, 3, 1}}, // -Z
    {{4, 5, 7, 6}}, // +Z
    {{0, 4, 6, 2}}, // -X
    {{1, 3, 7, 5}}, // +X
    {{0, 1, 5, 4}}, // -Y
    {{2, 6, 7, 3}}, // +Y
}};
static_assert(ABSORBING_MEDIUM_BOX_TRIANGLES == BOX_FACES * 2);
static_assert(ABSORBING_MEDIUM_BOX_TRIANGLES <= renderer::MAX_REFERENCE_MEDIUM_TRIANGLES);
/// Percent of light a channel keeps, for the readout.
constexpr float64 PERCENT = 100;
/// Significant digits of a readout percentage.
constexpr int32 PERCENT_DIGITS = 3;

/// Red, green and blue.
constexpr usize RGB_CHANNELS = 3;
const StaticArray<QString, RGB_CHANNELS> ABSORPTION_KEYS{{
    QStringLiteral("absorptionR"),
    QStringLiteral("absorptionG"),
    QStringLiteral("absorptionB"),
}};
const StaticArray<QString, RGB_CHANNELS> CHANNEL_NAMES{{QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")}};

[[nodiscard]] math::Vector3d ReadSize(const QJsonObject& values)
{
    return {values.value(QStringLiteral("width")).toDouble(),
            values.value(QStringLiteral("height")).toDouble(),
            values.value(QStringLiteral("depth")).toDouble()};
}

[[nodiscard]] float64 GetChannel(const math::Vector3d& value, usize channel)
{
    return channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z);
}

[[nodiscard]] bool ValidateValues(const QJsonObject& values, QString& outError)
{
    const math::Vector3d size = ReadSize(values);
    if (math::IsFinite(size.X) == false || math::IsFinite(size.Y) == false || math::IsFinite(size.Z) == false ||
        size.X <= 0 || size.Y <= 0 || size.Z <= 0)
    {
        outError = QStringLiteral("Absorbing Medium Width, Height and Depth must be finite and positive.");
        return false;
    }
    const math::Vector3d absorption = GetAbsorbingMediumAbsorption(values);
    for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
    {
        const float64 value = GetChannel(absorption, channel);
        if ((math::IsFinite(value) == true && value >= 0 &&
             value <= static_cast<float64>(format::MAX_MEDIUM_ABSORPTION_PER_WORLD)) == false)
        {
            outError = QStringLiteral("Absorbing Medium Absorption %1 must be between 0 and %2 per metre. Enter a "
                                      "coefficient in that range; 0 is vacuum.")
                           .arg(CHANNEL_NAMES[channel])
                           .arg(static_cast<float64>(format::MAX_MEDIUM_ABSORPTION_PER_WORLD));
            return false;
        }
    }
    return true;
}
} // namespace

QString GetAbsorbingMediumComponentId()
{
    return QStringLiteral("absorbingMedium");
}

void ApplyAbsorbingMediumLimits(QJsonObject& inoutComponent)
{
    QJsonArray properties = inoutComponent.value(QStringLiteral("properties")).toArray();
    for (qsizetype i = 0; i < properties.size(); ++i)
    {
        QJsonObject property = properties[i].toObject();
        for (const QString& key : ABSORPTION_KEYS)
        {
            if (property.value(QStringLiteral("id")).toString() == key)
            {
                property.insert(QStringLiteral("min"), 0);
                property.insert(QStringLiteral("max"), static_cast<float64>(format::MAX_MEDIUM_ABSORPTION_PER_WORLD));
            }
        }
        properties[i] = property;
    }
    inoutComponent.insert(QStringLiteral("properties"), properties);
}

math::Vector3d GetAbsorbingMediumAbsorption(const QJsonObject& values)
{
    return {values.value(ABSORPTION_KEYS[0]).toDouble(),
            values.value(ABSORPTION_KEYS[1]).toDouble(),
            values.value(ABSORPTION_KEYS[2]).toDouble()};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): component values, then the entity's Transform values
bool ValidateAbsorbingMedium(const QJsonObject& values, const QJsonObject& transformValues, QString& outError)
{
    if (ValidateValues(values, outError) == false)
    {
        return false;
    }
    // However the Transform turns the box, no corner lies farther from its centre than half the scaled
    // diagonal, so this bound holds for every rotation.
    const math::Vector3d size = ReadSize(values);
    const math::Vector3d scaledSize{size.X * transformValues.value(QStringLiteral("sx")).toDouble(),
                                    size.Y * transformValues.value(QStringLiteral("sy")).toDouble(),
                                    size.Z * transformValues.value(QStringLiteral("sz")).toDouble()};
    const float64 halfDiagonal = 0.5 * math::Length(scaledSize);
    const float64 limit = static_cast<float64>(renderer::MAX_REFERENCE_MEDIUM_COORDINATE_WORLD);
    for (const QString& axis : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")})
    {
        const float64 reach = math::Abs(transformValues.value(axis).toDouble()) + halfDiagonal;
        if ((math::IsFinite(reach) == true && reach <= limit) == false)
        {
            outError = QStringLiteral("The Absorbing Medium box reaches %1 m along %2, beyond the %3 m that RT1 "
                                      "admits. Move it toward the origin or make it smaller.")
                           .arg(FormatInspectorNumber(reach), axis.toUpper(), FormatInspectorNumber(limit));
            return false;
        }
    }
    return true;
}

bool BuildAbsorbingMediumBoundary(const QJsonObject& values,
                                  const math::Affine3& boxToWorld,
                                  QVector<math::Vector3>& outCornersWorld,
                                  QString& outError)
{
    if (ValidateValues(values, outError) == false)
    {
        return false;
    }
    const math::Vector3d size = ReadSize(values);
    const math::Vector3 half{static_cast<float32>(0.5 * size.X),
                             static_cast<float32>(0.5 * size.Y),
                             static_cast<float32>(0.5 * size.Z)};
    const float32 determinant =
        math::Dot(math::Cross(boxToWorld.Columns[0], boxToWorld.Columns[1]), boxToWorld.Columns[2]);
    if (math::IsFinite(boxToWorld) == false || math::IsFinite(determinant) == false || determinant == 0)
    {
        outError = QStringLiteral("The Absorbing Medium Transform must be finite with a nonzero scale.");
        return false;
    }
    // Transform each corner once, so the two triangles sharing an edge repeat bit-identical corners.
    StaticArray<math::Vector3, BOX_CORNERS> cornersWorld{};
    for (usize corner = 0; corner < BOX_CORNERS; ++corner)
    {
        const math::Vector3 cornerBox{(corner & X_CORNER_BIT) != 0 ? half.X : -half.X,
                                      (corner & Y_CORNER_BIT) != 0 ? half.Y : -half.Y,
                                      (corner & Z_CORNER_BIT) != 0 ? half.Z : -half.Z};
        cornersWorld[corner] = math::TransformPoint(boxToWorld, cornerBox);
        const math::Vector3& cornerWorld = cornersWorld[corner];
        const float32 limit = renderer::MAX_REFERENCE_MEDIUM_COORDINATE_WORLD;
        if (math::IsFinite(cornerWorld) == false || math::Abs(cornerWorld.X) > limit ||
            math::Abs(cornerWorld.Y) > limit || math::Abs(cornerWorld.Z) > limit)
        {
            outError = QStringLiteral("An Absorbing Medium corner lies beyond the %1 m that RT1 admits. Move the box "
                                      "toward the origin or make it smaller.")
                           .arg(FormatInspectorNumber(static_cast<float64>(limit)));
            return false;
        }
    }
    // A mirroring map reverses the winding seen from outside; swapping two corners restores it.
    const bool isMirrored = determinant < 0;
    QVector<math::Vector3> corners;
    corners.reserve(static_cast<qsizetype>(ABSORBING_MEDIUM_BOX_TRIANGLES) *
                    static_cast<qsizetype>(renderer::REFERENCE_MEDIUM_TRIANGLE_CORNERS));
    for (const StaticArray<usize, QUAD_CORNERS>& face : BOX_FACE_CORNERS)
    {
        for (usize triangle = 0; triangle < 2; ++triangle)
        {
            const usize second = face[triangle + 1];
            const usize third = face[triangle + 2];
            corners.append(cornersWorld[face[0]]);
            corners.append(cornersWorld[isMirrored == true ? third : second]);
            corners.append(cornersWorld[isMirrored == true ? second : third]);
        }
    }
    outCornersWorld = Move(corners);
    return true;
}

bool TryComputeAttenuationDistance(float64 absorptionPerWorld, float64& outDistanceWorld) noexcept
{
    if ((absorptionPerWorld > 0) == false)
    {
        return false;
    }
    outDistanceWorld = 1 / absorptionPerWorld;
    return true;
}

QString DescribeAbsorbingMedium(const math::Vector3d& absorptionPerWorld, float64 smallestExtentWorld)
{
    QStringList distances;
    QStringList transmittances;
    bool isVacuum = true;
    for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
    {
        const float64 absorption = GetChannel(absorptionPerWorld, channel);
        float64 distance = 0;
        if (TryComputeAttenuationDistance(absorption, distance) == true)
        {
            isVacuum = false;
            distances.append(QStringLiteral("%1 %2 m").arg(CHANNEL_NAMES[channel], FormatInspectorNumber(distance)));
        }
        else
        {
            distances.append(QStringLiteral("%1 vacuum (no attenuation)").arg(CHANNEL_NAMES[channel]));
        }
        const float64 kept = math::Exp(-absorption * smallestExtentWorld) * PERCENT;
        transmittances.append(
            QStringLiteral("%1 %2%").arg(CHANNEL_NAMES[channel], QString::number(kept, 'g', PERCENT_DIGITS)));
    }
    if (isVacuum == true)
    {
        return QStringLiteral("Vacuum: every absorption coefficient is zero, so light crosses the box unattenuated.");
    }
    QString text = QStringLiteral("Attenuation distance (light falls to 1/e, 37%): %1.")
                       .arg(distances.join(QStringLiteral(" · ")));
    if (smallestExtentWorld > 0)
    {
        text += QStringLiteral(" Across the box's shortest side (%1 m), light keeps %2.")
                    .arg(FormatInspectorNumber(smallestExtentWorld), transmittances.join(QStringLiteral(" · ")));
    }
    return text;
}
} // namespace ludus::editor
