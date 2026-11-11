#include "internal/world_probe_volume.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include "internal/inspector_fields.h"

#include <QStringList>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rr = graphics::renderer;
constexpr StaticArray<char, rr::GI_PROBE_DIMENSIONS> POSITION_AXES{'x', 'y', 'z'};
/// The covering spacing is rounded up to whole centimetres so it reads cleanly in the inspector.
constexpr float64 CENTIMETRES_PER_METRE = 100;
/// Most intervals an authored edge can hold: the largest edge at the smallest spacing.
constexpr float64 MAX_GI_PROBE_INTERVALS = MAX_GI_PROBE_VOLUME_SIZE_METRES / rr::MIN_GI_PROBE_SPACING_METRES;
constexpr int32 COVERAGE_DECIMALS = 2;

QString GetProbeCountKey(char axis)
{
    return QStringLiteral("count") + QChar::fromLatin1(axis).toUpper();
}

QString GetScaleKey(char axis)
{
    return QStringLiteral("s") + QChar::fromLatin1(axis);
}

[[nodiscard]] float32 GetAxis(const math::Vector3& vector, usize axis) noexcept
{
    return axis == 0 ? vector.X : axis == 1 ? vector.Y : vector.Z;
}

void SetAxis(math::Vector3& inoutVector, usize axis, float32 value) noexcept
{
    (axis == 0 ? inoutVector.X : axis == 1 ? inoutVector.Y : inoutVector.Z) = value;
}

[[nodiscard]] math::Vector3 ComputeGridExtentWorld(const rr::GiProbeGrid& grid) noexcept
{
    return {static_cast<float32>(grid.Counts[0] - 1) * grid.Spacing,
            static_cast<float32>(grid.Counts[1] - 1) * grid.Spacing,
            static_cast<float32>(grid.Counts[2] - 1) * grid.Spacing};
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

QString GetGiProbeLayoutKey()
{
    return QStringLiteral("layout");
}

QString GetGiProbeLayoutWord(GiProbeLayout layout)
{
    return layout == GiProbeLayout::Volume ? QStringLiteral("volume") : QStringLiteral("counts");
}

GiProbeLayout ReadWorldGiProbeLayout(const QJsonObject& components)
{
    const QJsonValue layout = components.value(GetGiProbeVolumeComponentId()).toObject().value(GetGiProbeLayoutKey());
    // Volumes authored before the layout existed stored their counts, so an absent word keeps them.
    return layout.toString() == GetGiProbeLayoutWord(GiProbeLayout::Volume) ? GiProbeLayout::Volume
                                                                            : GiProbeLayout::Counts;
}

bool WorldGiProbeVolume::IsCoverageLimited() const noexcept
{
    return RequiredCounts != Grid.Counts;
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
        QJsonObject{
            {QStringLiteral("id"), GetGiProbeLayoutKey()},
            {QStringLiteral("label"), QStringLiteral("Probe layout")},
            {QStringLiteral("type"), QStringLiteral("enum")},
            {QStringLiteral("choices"),
             QJsonArray{GetGiProbeLayoutWord(GiProbeLayout::Volume), GetGiProbeLayoutWord(GiProbeLayout::Counts)}},
            {QStringLiteral("default"), GetGiProbeLayoutWord(GiProbeLayout::Volume)},
            {QStringLiteral("unit"), QString()},
            {QStringLiteral("help"),
             QStringLiteral("volume: probes fill the Transform scale, read in metres, at the spacing. "
                            "counts: Probes along X, Y and Z set the size and the Transform scale is "
                            "not used.")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("spacing")},
                    {QStringLiteral("label"), QStringLiteral("Probe spacing")},
                    {QStringLiteral("type"), QStringLiteral("number")},
                    {QStringLiteral("default"), DEFAULT_GI_PROBE_SPACING_METRES},
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
            {QStringLiteral("default"), DEFAULT_GI_PROBES_PER_AXIS},
            {QStringLiteral("unit"), QString()},
            {QStringLiteral("min"), 2},
            {QStringLiteral("max"), static_cast<int32>(rr::MAX_GI_PROBE_AXIS_COUNT)},
            {QStringLiteral("help"),
             QStringLiteral("Number of sample points, including both ends. Used by the counts layout.")},
        });
    }
    inoutComponents.append(QJsonObject{
        {QStringLiteral("id"), GetGiProbeVolumeComponentId()},
        {QStringLiteral("label"), QStringLiteral("GI Probe Volume")},
        {QStringLiteral("required"), false},
        {QStringLiteral("help"),
         QStringLiteral("Transform position is the minimum corner and Transform scale the size, in metres. "
                        "Probes fill the volume from that corner at the spacing. Use Move to place it and "
                        "Scale or the face handles to resize it.")},
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

uint32 ComputeGiProbeCountForSize(float64 sizeMetres, float64 spacingMetres) noexcept
{
    const float64 spacing = math::Max(spacingMetres, rr::MIN_GI_PROBE_SPACING_METRES);
    // The nearest whole number of intervals, so an edge within half a spacing of the last probe plane keeps it.
    const float64 intervals = math::Floor(sizeMetres / spacing + 0.5);
    if (math::IsFinite(intervals) == false || intervals < 1)
    {
        return 2;
    }
    return static_cast<uint32>(math::Min(intervals, MAX_GI_PROBE_INTERVALS)) + 1;
}

rr::GiProbeGrid ComputeGiProbeGridForVolume(const math::Vector3& originWorld,
                                            const math::Vector3& sizeWorld,
                                            float32 spacingMetres,
                                            GiProbeCounts& outRequiredCounts) noexcept
{
    rr::GiProbeGrid grid;
    grid.OriginWorld = originWorld;
    grid.Spacing = spacingMetres;
    for (usize axis = 0; axis < rr::GI_PROBE_DIMENSIONS; ++axis)
    {
        const uint32 required = ComputeGiProbeCountForSize(static_cast<float64>(GetAxis(sizeWorld, axis)),
                                                           static_cast<float64>(spacingMetres));
        outRequiredCounts[axis] = required;
        grid.Counts[axis] = math::Min(required, rr::MAX_GI_PROBE_AXIS_COUNT);
    }
    return grid;
}

float64 ComputeGiProbeSpacingCoveringSize(const math::Vector3& sizeWorld) noexcept
{
    const float64 largestEdge = static_cast<float64>(math::Max(sizeWorld.X, math::Max(sizeWorld.Y, sizeWorld.Z)));
    const float64 spacing = largestEdge / static_cast<float64>(rr::MAX_GI_PROBE_AXIS_COUNT - 1);
    const float64 rounded = math::Ceil(spacing * CENTIMETRES_PER_METRE) / CENTIMETRES_PER_METRE;
    // Whole centimetres read well; fall back to the exact value when rounding would drop a probe on a tiny box.
    const float64 chosen =
        ComputeGiProbeCountForSize(largestEdge, rounded) == rr::MAX_GI_PROBE_AXIS_COUNT ? rounded : spacing;
    return math::Clamp(chosen, rr::MIN_GI_PROBE_SPACING_METRES, rr::MAX_GI_PROBE_SPACING_METRES);
}

bool ReadWorldGiProbeVolume(const QJsonObject& components, WorldGiProbeVolume& outVolume, QString& outError)
{
    const QJsonObject volume = components.value(GetGiProbeVolumeComponentId()).toObject();
    if (volume.isEmpty() == true)
    {
        outVolume = {};
        return true;
    }
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    const QJsonArray origin = ReadWorldGiProbeOrigin(components);
    WorldGiProbeVolume candidate;
    candidate.Layout = ReadWorldGiProbeLayout(components);
    candidate.IsEnabled = volume.value(QStringLiteral("enabled")).toBool();
    const math::Vector3 originWorld{static_cast<float32>(origin[0].toDouble()),
                                    static_cast<float32>(origin[1].toDouble()),
                                    static_cast<float32>(origin[2].toDouble())};
    const float32 spacing = static_cast<float32>(volume.value(QStringLiteral("spacing")).toDouble());
    if (candidate.Layout == GiProbeLayout::Volume)
    {
        for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
        {
            SetAxis(candidate.SizeWorld,
                    axis,
                    static_cast<float32>(transform.value(GetScaleKey(POSITION_AXES[axis])).toDouble()));
        }
        candidate.Grid =
            ComputeGiProbeGridForVolume(originWorld, candidate.SizeWorld, spacing, candidate.RequiredCounts);
    }
    else
    {
        candidate.Grid.OriginWorld = originWorld;
        candidate.Grid.Spacing = spacing;
        for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
        {
            candidate.Grid.Counts[axis] =
                static_cast<uint32>(volume.value(GetProbeCountKey(POSITION_AXES[axis])).toInt());
        }
        candidate.RequiredCounts = candidate.Grid.Counts;
        if (candidate.Grid.Counts[0] != 0)
        {
            candidate.SizeWorld = ComputeGridExtentWorld(candidate.Grid);
        }
    }
    if (rr::IsGiProbeGridValid(candidate.Grid) == false || candidate.Grid.Counts[0] == 0)
    {
        outError = candidate.Layout == GiProbeLayout::Volume
                       ? QStringLiteral("GI Probe Volume exceeds the World bounds. Move its Transform or reduce "
                                        "its scale or spacing.")
                       : QStringLiteral("GI Probe Volume exceeds the World bounds. Move its Transform or reduce "
                                        "probe count or spacing.");
        return false;
    }
    outVolume = candidate;
    return true;
}

bool ReadWorldGiProbeGrid(const QJsonObject& components,
                          rr::GiProbeGrid& outGrid,
                          QString& outError,
                          bool shouldIncludeDisabled)
{
    WorldGiProbeVolume volume;
    if (ReadWorldGiProbeVolume(components, volume, outError) == false)
    {
        return false;
    }
    outGrid = shouldIncludeDisabled == true || volume.IsEnabled == true ? volume.Grid : rr::GiProbeGrid{};
    return true;
}

math::Aabb3 ComputeWorldGiProbeBounds(const rr::GiProbeGrid& grid) noexcept
{
    return {grid.OriginWorld, grid.OriginWorld + ComputeGridExtentWorld(grid)};
}

math::Aabb3 ComputeWorldGiProbeVolumeBounds(const WorldGiProbeVolume& volume) noexcept
{
    return {volume.Grid.OriginWorld, volume.Grid.OriginWorld + volume.SizeWorld};
}

QString DescribeWorldGiProbeCoverage(const WorldGiProbeVolume& volume)
{
    if (volume.IsCoverageLimited() == false)
    {
        return QString();
    }
    QStringList shortEdges;
    for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
    {
        if (volume.RequiredCounts[axis] == volume.Grid.Counts[axis])
        {
            continue;
        }
        const float64 reachMetres =
            static_cast<float64>(static_cast<float32>(volume.Grid.Counts[axis] - 1) * volume.Grid.Spacing);
        shortEdges.append(QStringLiteral("%1 along %2 (%3 reach %4 of %5 m)")
                              .arg(volume.RequiredCounts[axis])
                              .arg(QChar::fromLatin1(POSITION_AXES[axis]).toUpper())
                              .arg(volume.Grid.Counts[axis])
                              .arg(reachMetres, 0, 'f', COVERAGE_DECIMALS)
                              .arg(static_cast<float64>(GetAxis(volume.SizeWorld, axis)), 0, 'f', COVERAGE_DECIMALS));
    }
    return QStringLiteral("Probe limit reached: the volume needs %1 at %2 m spacing. Set Probe spacing to %3 m "
                          "to reach every edge, or shrink the volume.")
        .arg(shortEdges.join(QStringLiteral(", ")),
             FormatInspectorNumber(static_cast<float64>(volume.Grid.Spacing)),
             FormatInspectorNumber(ComputeGiProbeSpacingCoveringSize(volume.SizeWorld)));
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

void WriteWorldGiProbeSpacing(QJsonObject& inoutComponents, float64 spacingMetres)
{
    const QString component = GetGiProbeVolumeComponentId();
    QJsonObject volume = inoutComponents.value(component).toObject();
    volume.insert(QStringLiteral("spacing"), spacingMetres);
    inoutComponents.insert(component, volume);
}

void WriteWorldGiProbeSize(QJsonObject& inoutComponents, usize axis, float64 sizeMetres)
{
    const QString key = QStringLiteral("transform");
    QJsonObject transform = inoutComponents.value(key).toObject();
    transform.insert(GetScaleKey(POSITION_AXES[axis]), sizeMetres);
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

void ResizeWorldGiProbeVolumeAxis(QJsonObject& inoutComponents, usize axis, float64 sizeMetres, bool isMinimumFace)
{
    if (isMinimumFace == true)
    {
        const qsizetype positionIndex = static_cast<qsizetype>(axis);
        const float64 previousSize = inoutComponents.value(QStringLiteral("transform"))
                                         .toObject()
                                         .value(GetScaleKey(POSITION_AXES[axis]))
                                         .toDouble();
        QJsonArray origin = ReadWorldGiProbeOrigin(inoutComponents);
        origin[positionIndex] = origin[positionIndex].toDouble() + (previousSize - sizeMetres);
        WriteWorldGiProbeOrigin(inoutComponents, origin);
    }
    WriteWorldGiProbeSize(inoutComponents, axis, sizeMetres);
}

void SynchronizeWorldGiProbeLayout(const QJsonObject& previousComponents, QJsonObject& inoutComponents)
{
    const GiProbeLayout previous = ReadWorldGiProbeLayout(previousComponents);
    const GiProbeLayout next = ReadWorldGiProbeLayout(inoutComponents);
    if (previous == next || inoutComponents.contains(GetGiProbeVolumeComponentId()) == false)
    {
        return;
    }
    const QString component = GetGiProbeVolumeComponentId();
    QJsonObject volume = inoutComponents.value(component).toObject();
    if (next == GiProbeLayout::Volume)
    {
        // Keep any scale edits made alongside the layout change. Untouched edges inherit the counts' extent.
        const QJsonObject previousTransform = previousComponents.value(QStringLiteral("transform")).toObject();
        const QJsonObject editedTransform = inoutComponents.value(QStringLiteral("transform")).toObject();
        const float64 spacing = volume.value(QStringLiteral("spacing")).toDouble();
        for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
        {
            const QString scaleKey = GetScaleKey(POSITION_AXES[axis]);
            if (editedTransform.value(scaleKey) == previousTransform.value(scaleKey))
            {
                const float64 intervals = volume.value(GetProbeCountKey(POSITION_AXES[axis])).toDouble() - 1;
                WriteWorldGiProbeSize(inoutComponents, axis, math::Max(intervals, 1.0) * spacing);
            }
        }
        return;
    }
    // The counts the Transform scale produced become authored, so the grid keeps its probes.
    QJsonObject volumeLayoutComponents = inoutComponents;
    volume.insert(GetGiProbeLayoutKey(), GetGiProbeLayoutWord(GiProbeLayout::Volume));
    volumeLayoutComponents.insert(component, volume);
    WorldGiProbeVolume derived;
    QString error;
    if (ReadWorldGiProbeVolume(volumeLayoutComponents, derived, error) == false)
    {
        return;
    }
    volume.insert(GetGiProbeLayoutKey(), GetGiProbeLayoutWord(GiProbeLayout::Counts));
    for (usize axis = 0; axis < POSITION_AXES.GetSize(); ++axis)
    {
        volume.insert(GetProbeCountKey(POSITION_AXES[axis]), static_cast<int32>(derived.Grid.Counts[axis]));
    }
    inoutComponents.insert(component, volume);
}
} // namespace ludus::editor
