#include "internal/world_probe_volume.h"

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rr = graphics::renderer;
constexpr int32 DEFAULT_PROBES_PER_AXIS = 4;
} // namespace

QString GetGiProbeVolumeComponentId()
{
    return QStringLiteral("giProbeVolume");
}

QString GetGiProbeReceiverComponentId()
{
    return QStringLiteral("giProbeReceiver");
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
    for (const QChar axis : {QLatin1Char('X'), QLatin1Char('Y'), QLatin1Char('Z')})
    {
        properties.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("count") + axis},
            {QStringLiteral("label"), QStringLiteral("Probes along ") + axis},
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
                        "The grid follows the World axes. Bake GI uses always-on baked lights.")},
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

bool ReadWorldGiProbeGrid(const QJsonObject& components, rr::GiProbeGrid& outGrid, QString& outError)
{
    const QJsonObject volume = components.value(GetGiProbeVolumeComponentId()).toObject();
    if (volume.isEmpty() == true)
    {
        outGrid = {};
        return true;
    }
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    rr::GiProbeGrid grid;
    grid.OriginWorld = {static_cast<float32>(transform.value(QStringLiteral("x")).toDouble()),
                        static_cast<float32>(transform.value(QStringLiteral("y")).toDouble()),
                        static_cast<float32>(transform.value(QStringLiteral("z")).toDouble())};
    grid.Spacing = static_cast<float32>(volume.value(QStringLiteral("spacing")).toDouble());
    grid.Counts = {static_cast<uint32>(volume.value(QStringLiteral("countX")).toInt()),
                   static_cast<uint32>(volume.value(QStringLiteral("countY")).toInt()),
                   static_cast<uint32>(volume.value(QStringLiteral("countZ")).toInt())};
    if (rr::IsGiProbeGridValid(grid) == false || grid.Counts[0] == 0)
    {
        outError = QStringLiteral("GI Probe Volume exceeds the World bounds. Move its Transform or reduce probe count "
                                  "or spacing.");
        return false;
    }
    outGrid = volume.value(QStringLiteral("enabled")).toBool() == true ? grid : rr::GiProbeGrid{};
    return true;
}
} // namespace ludus::editor
