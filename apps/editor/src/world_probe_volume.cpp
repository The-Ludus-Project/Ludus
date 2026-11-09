#include "internal/world_probe_volume.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rr = graphics::renderer;
constexpr int32 DEFAULT_PROBES_PER_AXIS = 4;
constexpr StaticArray<char, rr::GI_PROBE_DIMENSIONS> POSITION_AXES{'x', 'y', 'z'};

QString GetProbeCountKey(char axis)
{
    return QStringLiteral("count") + QChar::fromLatin1(axis).toUpper();
}
} // namespace

QString GetGiProbeVolumeComponentId()
{
    return QStringLiteral("giProbeVolume");
}

QString GetGiProbeReceiverComponentId()
{
    return QStringLiteral("giProbeReceiver");
}

bool IsStandaloneWorldGiProbeVolume(const QJsonObject& entity, const QJsonObject& components)
{
    return components.size() == 2 && components.contains(GetGiProbeVolumeComponentId()) == true &&
           components.contains(QStringLiteral("transform")) == true &&
           entity.contains(QStringLiteral("scene")) == false;
}

void AppendGiProbeComponents(QJsonArray& inoutComponents)
{
    QJsonArray properties{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("enabled")},
                    {QStringLiteral("label"), QStringLiteral("Enable baked GI")},
                    {QStringLiteral("type"), QStringLiteral("boolean")},
                    {QStringLiteral("default"), false},
                    {QStringLiteral("unit"), QString()},
                    {QStringLiteral("help"), QStringLiteral("Use the volume in the native Lit viewport.")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("spacing")},
                    {QStringLiteral("label"), QStringLiteral("Probe spacing")},
                    {QStringLiteral("type"), QStringLiteral("number")},
                    {QStringLiteral("default"), 1},
                    {QStringLiteral("unit"), QStringLiteral("m")},
                    {QStringLiteral("min"), rr::MIN_GI_PROBE_SPACING_METRES},
                    {QStringLiteral("max"), rr::MAX_GI_PROBE_SPACING_METRES},
                    {QStringLiteral("help"), QStringLiteral("Distance between neighbouring probes.")}},
    };
    for (const char axis : POSITION_AXES)
    {
        properties.append(QJsonObject{
            {QStringLiteral("id"), GetProbeCountKey(axis)},
            {QStringLiteral("label"), QStringLiteral("Probes along ") + QChar::fromLatin1(axis).toUpper()},
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("default"), DEFAULT_PROBES_PER_AXIS},
            {QStringLiteral("unit"), QString()},
            {QStringLiteral("min"), 2},
            {QStringLiteral("max"), static_cast<int32>(rr::MAX_GI_PROBE_AXIS_COUNT)},
            {QStringLiteral("help"), QStringLiteral("Number of sample points, including both ends.")},
        });
    }
    inoutComponents.append(QJsonObject{
        {QStringLiteral("id"), GetGiProbeVolumeComponentId()},
        {QStringLiteral("label"), QStringLiteral("GI Probe Volume")},
        {QStringLiteral("required"), false},
        {QStringLiteral("help"),
         QStringLiteral("Transform position is the minimum corner, in metres. "
                        "Use Move in the viewport to position the box or drag a face to change its probe count.")},
        {QStringLiteral("properties"), properties},
    });
    inoutComponents.append(QJsonObject{
        {QStringLiteral("id"), GetGiProbeReceiverComponentId()},
        {QStringLiteral("label"), QStringLiteral("GI Probe Receiver")},
        {QStringLiteral("required"), false},
        {QStringLiteral("help"),
         QStringLiteral("Use the GI volume for this moving object. Its geometry is excluded "
                        "from the static lighting bake, and direct lighting stays realtime.")},
        {QStringLiteral("properties"), QJsonArray{}},
    });
}

bool ReadWorldGiProbeGrid(const QJsonObject& components,
                          rr::GiProbeGrid& outGrid,
                          QString& outError,
                          bool shouldIncludeDisabled)
{
    const QJsonObject volume = components.value(GetGiProbeVolumeComponentId()).toObject();
    if (volume.isEmpty() == true)
    {
        outGrid = {};
        return true;
    }
    const QJsonArray origin = ReadWorldGiProbeOrigin(components);
    rr::GiProbeGrid grid;
    grid.OriginWorld = {static_cast<float32>(origin[0].toDouble()),
                        static_cast<float32>(origin[1].toDouble()),
                        static_cast<float32>(origin[2].toDouble())};
    grid.Spacing = static_cast<float32>(volume.value(QStringLiteral("spacing")).toDouble());
    for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
    {
        grid.Counts[axis] = static_cast<uint32>(volume.value(GetProbeCountKey(POSITION_AXES[axis])).toInt());
    }
    if (rr::IsGiProbeGridValid(grid) == false || grid.Counts[0] == 0)
    {
        outError = QStringLiteral("GI Probe Volume exceeds the World bounds. Move its Transform or reduce probe count "
                                  "or spacing.");
        return false;
    }
    outGrid = shouldIncludeDisabled == true || volume.value(QStringLiteral("enabled")).toBool() == true
                  ? grid
                  : rr::GiProbeGrid{};
    return true;
}

math::Aabb3 ComputeWorldGiProbeBounds(const rr::GiProbeGrid& grid) noexcept
{
    const math::Vector3 extentWorld{static_cast<float32>(grid.Counts[0] - 1) * grid.Spacing,
                                    static_cast<float32>(grid.Counts[1] - 1) * grid.Spacing,
                                    static_cast<float32>(grid.Counts[2] - 1) * grid.Spacing};
    return {grid.OriginWorld, grid.OriginWorld + extentWorld};
}

QJsonArray ReadWorldGiProbeOrigin(const QJsonObject& components)
{
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    QJsonArray origin;
    for (const char axis : POSITION_AXES)
    {
        origin.append(transform.value(QString(QLatin1Char(axis))));
    }
    return origin;
}

void WriteWorldGiProbeOrigin(QJsonObject& inoutComponents, const QJsonArray& origin)
{
    const QString key = QStringLiteral("transform");
    QJsonObject transform = inoutComponents.value(key).toObject();
    for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
    {
        transform.insert(QString(QLatin1Char(POSITION_AXES[axis])), origin[static_cast<qsizetype>(axis)]);
    }
    inoutComponents.insert(key, transform);
}

void ResizeWorldGiProbeAxis(QJsonObject& inoutComponents, usize axis, uint32 count, bool isMinimumFace)
{
    const QString component = GetGiProbeVolumeComponentId();
    QJsonObject volume = inoutComponents.value(component).toObject();
    const QString countKey = GetProbeCountKey(POSITION_AXES[axis]);
    if (isMinimumFace == true)
    {
        const qsizetype positionIndex = static_cast<qsizetype>(axis);
        const float64 intervals = volume.value(countKey).toDouble() - static_cast<float64>(count);
        QJsonArray origin = ReadWorldGiProbeOrigin(inoutComponents);
        origin[positionIndex] =
            origin[positionIndex].toDouble() + intervals * volume.value(QStringLiteral("spacing")).toDouble();
        WriteWorldGiProbeOrigin(inoutComponents, origin);
    }
    volume.insert(countKey, static_cast<int32>(count));
    inoutComponents.insert(component, volume);
}
} // namespace ludus::editor
