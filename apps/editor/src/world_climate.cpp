// Authored World climate: reads the "climate" root member, validates it with the engine cooker and names the
// failing record with a repair. The cooker owns every limit and unit (Ludus::WeatherFormat); this file owns only
// the JSON spelling. See docs/modules/gameplay/weather.md#author-a-world-climate.
#include "internal/world_climate.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QStringList>

namespace ludus::editor
{
namespace
{
using namespace Qt::StringLiterals;
namespace cook = content::weather;
namespace format = content::weather_format;
using foundation::float64;
using foundation::int32;
using foundation::uint32;
using foundation::uint8;

/// Largest integer an ID, revision or tick count can hold.
constexpr float64 MAX_UINT32_VALUE = 4294967295.0;
/// Range of an authored priority.
constexpr float64 MIN_INT32_VALUE = -2147483648.0;
constexpr float64 MAX_INT32_VALUE = 2147483647.0;
constexpr foundation::usize VECTOR_COMPONENTS = 3;

/// The display name of one authored record; a distinct type so it cannot be confused with an error message.
struct RecordName final
{
    QString Text;
};

struct ParsedClimate final
{
    QList<cook::WeatherPresetSource> Presets;
    QStringList PresetNames;
    QList<QList<cook::WeatherScheduleEntrySource>> Entries;
    QList<cook::WeatherScheduleSource> Schedules;
    QStringList ScheduleNames;
    QList<cook::WeatherRegionSource> Regions;
    QStringList RegionNames;
    cook::ClimateSource Source;
};

bool Fail(QString& outError, const QString& message)
{
    outError = message;
    return false;
}

bool HasExactly(const QJsonObject& object, const QStringList& fields)
{
    if (object.size() != fields.size())
    {
        return false;
    }
    for (const QString& field : fields)
    {
        if (object.contains(field) == false)
        {
            return false;
        }
    }
    return true;
}

bool TryReadNumber(const QJsonObject& object, QLatin1StringView key, float64& outValue)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble() == false || foundation::math::IsFinite(value.toDouble()) == false)
    {
        return false;
    }
    outValue = value.toDouble();
    return true;
}

bool TryReadInteger(const QJsonObject& object,
                    QLatin1StringView key,
                    float64 minimum,
                    float64 maximum,
                    float64& outValue)
{
    float64 value = 0;
    if (TryReadNumber(object, key, value) == false || foundation::math::Floor(value) != value || value < minimum ||
        value > maximum)
    {
        return false;
    }
    outValue = value;
    return true;
}

bool TryReadUnsigned(const QJsonObject& object, QLatin1StringView key, uint32& outValue)
{
    float64 value = 0;
    if (TryReadInteger(object, key, 0, MAX_UINT32_VALUE, value) == false)
    {
        return false;
    }
    outValue = static_cast<uint32>(value);
    return true;
}

bool TryReadVector(const QJsonObject& object, QLatin1StringView key, foundation::math::Vector3d& outValue)
{
    const QJsonArray array = object.value(key).toArray();
    if (object.value(key).isArray() == false || array.size() != static_cast<qsizetype>(VECTOR_COMPONENTS))
    {
        return false;
    }
    foundation::StaticArray<float64, VECTOR_COMPONENTS> components{};
    for (foundation::usize i = 0; i < VECTOR_COMPONENTS; ++i)
    {
        const QJsonValue component = array[static_cast<qsizetype>(i)];
        if (component.isDouble() == false || foundation::math::IsFinite(component.toDouble()) == false)
        {
            return false;
        }
        components[i] = component.toDouble();
    }
    outValue = {components[0], components[1], components[2]};
    return true;
}

bool TryReadBounds(const QJsonObject& object, QLatin1StringView key, cook::WeatherBoundsMetres& outBounds)
{
    const QJsonObject bounds = object.value(key).toObject();
    cook::WeatherBoundsMetres result;
    if (object.value(key).isObject() == false ||
        HasExactly(bounds, {u"minimumX"_s, u"minimumZ"_s, u"maximumX"_s, u"maximumZ"_s}) == false ||
        TryReadNumber(bounds, "minimumX"_L1, result.MinimumX) == false ||
        TryReadNumber(bounds, "minimumZ"_L1, result.MinimumZ) == false ||
        TryReadNumber(bounds, "maximumX"_L1, result.MaximumX) == false ||
        TryReadNumber(bounds, "maximumZ"_L1, result.MaximumZ) == false)
    {
        return false;
    }
    outBounds = result;
    return true;
}

bool TryReadName(const QJsonObject& object, RecordName& outName)
{
    const QJsonValue value = object.value("name"_L1);
    const QString name = value.toString().trimmed();
    if (value.isString() == false || name.isEmpty() == true || name.size() > MAX_WORLD_CLIMATE_NAME_LENGTH)
    {
        return false;
    }
    outName.Text = name;
    return true;
}

QString DescribeNameRule()
{
    return QStringLiteral("a name of 1 to %1 characters").arg(MAX_WORLD_CLIMATE_NAME_LENGTH);
}

bool TryReadPreset(const QJsonValue& value,
                   cook::WeatherPresetSource& outPreset,
                   RecordName& outName,
                   QString& outError)
{
    const QJsonObject preset = value.toObject();
    const QStringList fields{u"id"_s,
                             u"revision"_s,
                             u"name"_s,
                             u"temperature"_s,
                             u"humidity"_s,
                             u"groundWind"_s,
                             u"cloudWind"_s,
                             u"cloudCoverage"_s,
                             u"cloudType"_s,
                             u"cloudDensity"_s,
                             u"fogExtinction"_s,
                             u"fogScaleHeight"_s,
                             u"precipitation"_s};
    cook::WeatherPresetSource result;
    if (value.isObject() == false || HasExactly(preset, fields) == false || TryReadName(preset, outName) == false ||
        TryReadUnsigned(preset, "id"_L1, result.Id) == false ||
        TryReadUnsigned(preset, "revision"_L1, result.Revision) == false ||
        TryReadNumber(preset, "temperature"_L1, result.TemperatureCelsius) == false ||
        TryReadNumber(preset, "humidity"_L1, result.RelativeHumidity) == false ||
        TryReadVector(preset, "groundWind"_L1, result.GroundWindWorld) == false ||
        TryReadVector(preset, "cloudWind"_L1, result.CloudWindWorld) == false ||
        TryReadNumber(preset, "cloudCoverage"_L1, result.CloudCoverage) == false ||
        TryReadNumber(preset, "cloudType"_L1, result.CloudType) == false ||
        TryReadNumber(preset, "cloudDensity"_L1, result.CloudDensityScale) == false ||
        TryReadNumber(preset, "fogExtinction"_L1, result.FogExtinctionPerMetre) == false ||
        TryReadNumber(preset, "fogScaleHeight"_L1, result.FogScaleHeightMetres) == false ||
        TryReadNumber(preset, "precipitation"_L1, result.PrecipitationMillimetresPerHour) == false)
    {
        return Fail(outError,
                    QStringLiteral("Each climate preset needs an id, a revision, %1, finite channel numbers and "
                                   "three-component winds (%2).")
                        .arg(DescribeNameRule(), fields.join(", "_L1)));
    }
    outPreset = result;
    return true;
}

bool TryReadSchedule(const QJsonValue& value,
                     cook::WeatherScheduleSource& outSchedule,
                     QList<cook::WeatherScheduleEntrySource>& outEntries,
                     RecordName& outName,
                     QString& outError)
{
    const QJsonObject schedule = value.toObject();
    cook::WeatherScheduleSource result;
    const QJsonArray entries = schedule.value("entries"_L1).toArray();
    if (value.isObject() == false ||
        HasExactly(schedule, {u"id"_s, u"name"_s, u"loopStart"_s, u"entries"_s}) == false ||
        TryReadName(schedule, outName) == false || TryReadUnsigned(schedule, "id"_L1, result.Id) == false ||
        TryReadUnsigned(schedule, "loopStart"_L1, result.LoopStartEntry) == false ||
        schedule.value("entries"_L1).isArray() == false)
    {
        return Fail(outError,
                    QStringLiteral("Each climate schedule needs an id, %1, a loopStart entry index and an entries "
                                   "list.")
                        .arg(DescribeNameRule()));
    }
    if (entries.size() > static_cast<qsizetype>(format::MAX_WEATHER_SCHEDULE_ENTRIES))
    {
        return Fail(outError,
                    QStringLiteral("Schedule “%1” has %2 entries; a climate holds at most %3. Remove entries.")
                        .arg(outName.Text)
                        .arg(entries.size())
                        .arg(format::MAX_WEATHER_SCHEDULE_ENTRIES));
    }
    QList<cook::WeatherScheduleEntrySource> parsed;
    for (const QJsonValue entryValue : entries)
    {
        const QJsonObject entry = entryValue.toObject();
        cook::WeatherScheduleEntrySource item;
        if (entryValue.isObject() == false || HasExactly(entry, {u"preset"_s, u"hold"_s, u"blend"_s}) == false ||
            TryReadUnsigned(entry, "preset"_L1, item.PresetId) == false ||
            TryReadUnsigned(entry, "hold"_L1, item.HoldTicks) == false ||
            TryReadUnsigned(entry, "blend"_L1, item.BlendTicks) == false)
        {
            return Fail(outError,
                        QStringLiteral("Each entry of schedule “%1” needs a preset id and whole hold and blend "
                                       "tick counts.")
                            .arg(outName.Text));
        }
        parsed.append(item);
    }
    outSchedule = result;
    outEntries = foundation::Move(parsed);
    return true;
}

bool TryReadRegion(const QJsonValue& value,
                   cook::WeatherRegionSource& outRegion,
                   RecordName& outName,
                   QString& outError)
{
    const QJsonObject region = value.toObject();
    cook::WeatherRegionSource result;
    float64 priority = 0;
    if (value.isObject() == false ||
        HasExactly(region, {u"id"_s, u"name"_s, u"priority"_s, u"schedule"_s, u"bounds"_s, u"feather"_s}) == false ||
        TryReadName(region, outName) == false || TryReadUnsigned(region, "id"_L1, result.Id) == false ||
        TryReadInteger(region, "priority"_L1, MIN_INT32_VALUE, MAX_INT32_VALUE, priority) == false ||
        TryReadUnsigned(region, "schedule"_L1, result.ScheduleId) == false ||
        TryReadBounds(region, "bounds"_L1, result.BoundsWorld) == false ||
        TryReadNumber(region, "feather"_L1, result.FeatherMetres) == false)
    {
        return Fail(outError,
                    QStringLiteral("Each climate region needs an id, %1, a whole priority, a schedule id, World "
                                   "bounds in metres (minimumX, minimumZ, maximumX, maximumZ) and a feather "
                                   "distance in metres.")
                        .arg(DescribeNameRule()));
    }
    result.Priority = static_cast<int32>(priority);
    outRegion = result;
    return true;
}

bool TryParseClimate(const QJsonValue& value, ParsedClimate& outClimate, QString& outError)
{
    const QJsonObject climate = value.toObject();
    ParsedClimate parsed;
    float64 version = 0;
    if (value.isObject() == false ||
        HasExactly(climate,
                   {u"version"_s,
                    u"id"_s,
                    u"revision"_s,
                    u"domain"_s,
                    u"defaultSchedule"_s,
                    u"presets"_s,
                    u"schedules"_s,
                    u"regions"_s}) == false ||
        TryReadInteger(climate, "version"_L1, 0, MAX_UINT32_VALUE, version) == false)
    {
        return Fail(outError,
                    QStringLiteral("The World climate needs version, id, revision, domain, defaultSchedule, presets, "
                                   "schedules and regions."));
    }
    if (version != WORLD_CLIMATE_VERSION)
    {
        return Fail(outError,
                    QStringLiteral("This Editor reads World climate version %1, not %2. Open the World in the Editor "
                                   "version that saved it.")
                        .arg(WORLD_CLIMATE_VERSION)
                        .arg(version));
    }
    if (TryReadUnsigned(climate, "id"_L1, parsed.Source.Id) == false ||
        TryReadUnsigned(climate, "revision"_L1, parsed.Source.Revision) == false ||
        TryReadUnsigned(climate, "defaultSchedule"_L1, parsed.Source.DefaultScheduleId) == false ||
        TryReadBounds(climate, "domain"_L1, parsed.Source.DomainWorld) == false ||
        climate.value("presets"_L1).isArray() == false || climate.value("schedules"_L1).isArray() == false ||
        climate.value("regions"_L1).isArray() == false)
    {
        return Fail(outError,
                    QStringLiteral("The World climate needs whole id, revision and defaultSchedule numbers, a domain "
                                   "in World metres and presets, schedules and regions lists."));
    }
    const QJsonArray presets = climate.value("presets"_L1).toArray();
    const QJsonArray schedules = climate.value("schedules"_L1).toArray();
    const QJsonArray regions = climate.value("regions"_L1).toArray();
    if (presets.size() > static_cast<qsizetype>(format::MAX_WEATHER_PRESETS) ||
        schedules.size() > static_cast<qsizetype>(format::MAX_WEATHER_SCHEDULES) ||
        regions.size() > static_cast<qsizetype>(format::MAX_WEATHER_REGIONS))
    {
        return Fail(outError,
                    QStringLiteral("The World climate has %1 presets, %2 schedules and %3 regions; it holds at most "
                                   "%4, %5 and %6. Remove the extra records.")
                        .arg(presets.size())
                        .arg(schedules.size())
                        .arg(regions.size())
                        .arg(format::MAX_WEATHER_PRESETS)
                        .arg(format::MAX_WEATHER_SCHEDULES)
                        .arg(format::MAX_WEATHER_REGIONS));
    }
    for (const QJsonValue preset : presets)
    {
        cook::WeatherPresetSource source;
        RecordName name;
        if (TryReadPreset(preset, source, name, outError) == false)
        {
            return false;
        }
        parsed.Presets.append(source);
        parsed.PresetNames.append(name.Text);
    }
    for (const QJsonValue schedule : schedules)
    {
        cook::WeatherScheduleSource source;
        QList<cook::WeatherScheduleEntrySource> entries;
        RecordName name;
        if (TryReadSchedule(schedule, source, entries, name, outError) == false)
        {
            return false;
        }
        parsed.Schedules.append(source);
        parsed.Entries.append(entries);
        parsed.ScheduleNames.append(name.Text);
    }
    for (const QJsonValue region : regions)
    {
        cook::WeatherRegionSource source;
        RecordName name;
        if (TryReadRegion(region, source, name, outError) == false)
        {
            return false;
        }
        parsed.Regions.append(source);
        parsed.RegionNames.append(name.Text);
    }
    outClimate = foundation::Move(parsed);
    return true;
}

QString DescribeChannel(uint32 channel)
{
    switch (static_cast<format::WeatherChannel>(channel))
    {
        case format::WeatherChannel::Temperature:
            return QStringLiteral("temperature must be −100 to 100 °C");
        case format::WeatherChannel::RelativeHumidity:
            return QStringLiteral("humidity must be 0 to 1");
        case format::WeatherChannel::GroundWindX:
        case format::WeatherChannel::GroundWindY:
        case format::WeatherChannel::GroundWindZ:
            return QStringLiteral("each ground wind component must be −150 to 150 m/s");
        case format::WeatherChannel::CloudWindX:
        case format::WeatherChannel::CloudWindY:
        case format::WeatherChannel::CloudWindZ:
            return QStringLiteral("each cloud wind component must be −150 to 150 m/s");
        case format::WeatherChannel::CloudCoverage:
            return QStringLiteral("cloud coverage must be 0 to 1");
        case format::WeatherChannel::CloudType:
            return QStringLiteral("cloud type must be 0 (stratiform) to 1 (cumuliform)");
        case format::WeatherChannel::CloudDensityScale:
            return QStringLiteral("cloud density must be 0 to 8");
        case format::WeatherChannel::FogExtinction:
            return QStringLiteral("fog extinction must be 0 to 1 per metre");
        case format::WeatherChannel::FogScaleHeight:
            return QStringLiteral("fog scale height must be 0 to 20000 m");
        case format::WeatherChannel::Precipitation:
            return QStringLiteral("precipitation must be 0 to 500 mm/h");
    }
    return QStringLiteral("a channel is out of range");
}

template <typename RecordList>
QString FindName(const QStringList& names, const RecordList& records, uint32 id)
{
    for (qsizetype i = 0; i < records.size(); ++i)
    {
        if (records[i].Id == id)
        {
            return names[i];
        }
    }
    return QStringLiteral("ID %1").arg(id);
}

QString DescribeCookFailure(const ParsedClimate& climate, const cook::ClimateCookResult& result)
{
    switch (result.Subject)
    {
        case cook::ClimateCookSubject::Preset: {
            const QString name = FindName(climate.PresetNames, climate.Presets, result.SubjectId);
            switch (result.Status)
            {
                case cook::ClimateCookStatus::ValueOutOfRange:
                    return QStringLiteral("Preset “%1”: %2. Enter a value in that range.")
                        .arg(name, DescribeChannel(result.Detail));
                case cook::ClimateCookStatus::DuplicateId:
                    return QStringLiteral("Two presets use ID %1. Give each preset its own ID.").arg(result.SubjectId);
                default:
                    return QStringLiteral("Preset “%1” needs a nonzero ID and revision.").arg(name);
            }
        }
        case cook::ClimateCookSubject::Schedule: {
            const QString name = FindName(climate.ScheduleNames, climate.Schedules, result.SubjectId);
            if (result.Status == cook::ClimateCookStatus::DuplicateId)
            {
                return QStringLiteral("Two schedules use ID %1. Give each schedule its own ID.").arg(result.SubjectId);
            }
            if (result.Status == cook::ClimateCookStatus::CapacityExceeded)
            {
                return QStringLiteral("The climate's schedules have more than %1 entries together. Remove entries from "
                                      "“%2” or another schedule.")
                    .arg(format::MAX_WEATHER_SCHEDULE_ENTRIES)
                    .arg(name);
            }
            return QStringLiteral(
                       "Schedule “%1” needs a nonzero ID, at least one entry and a loop start that names one "
                       "of its entries.")
                .arg(name);
        }
        case cook::ClimateCookSubject::ScheduleEntry: {
            const QString name = FindName(climate.ScheduleNames, climate.Schedules, result.SubjectId);
            if (result.Status == cook::ClimateCookStatus::MissingReference)
            {
                return QStringLiteral("Entry %1 of schedule “%2” names a preset that does not exist. Choose one of the "
                                      "climate's presets.")
                    .arg(result.Detail + 1)
                    .arg(name);
            }
            return QStringLiteral(
                       "Entry %1 of schedule “%2” lasts zero ticks. Give it a hold or a blend of at least one "
                       "tick.")
                .arg(result.Detail + 1)
                .arg(name);
        }
        case cook::ClimateCookSubject::Region: {
            const QString name = FindName(climate.RegionNames, climate.Regions, result.SubjectId);
            switch (result.Status)
            {
                case cook::ClimateCookStatus::MissingReference:
                    return QStringLiteral(
                               "Region “%1” uses a schedule that does not exist. Choose one of the climate's "
                               "schedules.")
                        .arg(name);
                case cook::ClimateCookStatus::InvalidBounds:
                    return QStringLiteral(
                               "Region “%1” must be a nonempty rectangle inside the climate domain. Shrink the "
                               "region or enlarge the domain.")
                        .arg(name);
                case cook::ClimateCookStatus::ValueOutOfRange:
                    return QStringLiteral("Region “%1” needs a feather distance of zero or more metres.").arg(name);
                case cook::ClimateCookStatus::DuplicateId:
                    return QStringLiteral("Two regions use ID %1. Give each region its own ID.").arg(result.SubjectId);
                default:
                    return QStringLiteral("Region “%1” needs a nonzero ID.").arg(name);
            }
        }
        case cook::ClimateCookSubject::Climate:
            break;
    }
    switch (result.Status)
    {
        case cook::ClimateCookStatus::InvalidBounds:
            return QStringLiteral("The climate domain must be a nonempty rectangle within ±10,000 km of the World "
                                  "origin.");
        case cook::ClimateCookStatus::MissingReference:
            return QStringLiteral("The climate's default schedule does not exist. Choose one of its schedules.");
        case cook::ClimateCookStatus::CapacityExceeded:
            return QStringLiteral("The climate has too many presets, schedules or regions.");
        case cook::ClimateCookStatus::AllocationFailed:
            return QStringLiteral("Memory for the cooked climate could not be allocated. Close other documents and "
                                  "try again.");
        default:
            return QStringLiteral("The climate needs a nonzero ID and revision, at least one preset and at least one "
                                  "schedule.");
    }
}

/// The member with every revision removed, to tell an authored change from an unchanged commit.
QJsonObject WithoutRevisions(QJsonObject climate)
{
    climate.remove("revision"_L1);
    QJsonArray presets = climate.value("presets"_L1).toArray();
    for (qsizetype i = 0; i < presets.size(); ++i)
    {
        QJsonObject preset = presets[i].toObject();
        preset.remove("revision"_L1);
        presets[i] = preset;
    }
    climate.insert("presets"_L1, presets);
    return climate;
}

uint32 GetRevision(const QJsonObject& object)
{
    const float64 value = object.value("revision"_L1).toDouble();
    return value >= 1 && value < MAX_UINT32_VALUE ? static_cast<uint32>(value) : 0;
}
} // namespace

bool TryCookWorldClimate(const QJsonValue& climate, foundation::Array<uint8>& outBytes, QString& outError)
{
    ParsedClimate parsed;
    if (TryParseClimate(climate, parsed, outError) == false)
    {
        return false;
    }
    for (qsizetype i = 0; i < parsed.Schedules.size(); ++i)
    {
        parsed.Schedules[i].Entries = {parsed.Entries[i].constData(),
                                       static_cast<foundation::usize>(parsed.Entries[i].size())};
    }
    parsed.Source.TicksPerSecond = content::scene_format::PLAYABLE_WORLD_TICKS_PER_SECOND;
    parsed.Source.Presets = {parsed.Presets.constData(), static_cast<foundation::usize>(parsed.Presets.size())};
    parsed.Source.Schedules = {parsed.Schedules.constData(), static_cast<foundation::usize>(parsed.Schedules.size())};
    parsed.Source.Regions = {parsed.Regions.constData(), static_cast<foundation::usize>(parsed.Regions.size())};
    foundation::Array<uint8> bytes;
    const cook::ClimateCookResult result = cook::TryCookClimate(parsed.Source, bytes);
    if (result.Status != cook::ClimateCookStatus::Success)
    {
        return Fail(outError, DescribeCookFailure(parsed, result));
    }
    outBytes = foundation::Move(bytes);
    outError.clear();
    return true;
}

QJsonObject ReviseWorldClimate(const QJsonValue& previous, const QJsonObject& edited)
{
    QJsonObject revised = edited;
    const QJsonObject before = previous.toObject();
    QHash<qint64, QJsonObject> previousPresets;
    for (const QJsonValue preset : before.value("presets"_L1).toArray())
    {
        previousPresets.insert(preset.toObject().value("id"_L1).toInteger(), preset.toObject());
    }
    QJsonArray presets = revised.value("presets"_L1).toArray();
    for (qsizetype i = 0; i < presets.size(); ++i)
    {
        QJsonObject preset = presets[i].toObject();
        const qint64 id = preset.value("id"_L1).toInteger();
        const QJsonObject old = previousPresets.value(id);
        uint32 revision = 1;
        if (previousPresets.contains(id) == true)
        {
            QJsonObject oldValues = old;
            QJsonObject newValues = preset;
            oldValues.remove("revision"_L1);
            newValues.remove("revision"_L1);
            revision = oldValues == newValues ? GetRevision(old) : GetRevision(old) + 1;
        }
        preset.insert("revision"_L1, static_cast<qint64>(revision == 0 ? 1 : revision));
        presets[i] = preset;
    }
    revised.insert("presets"_L1, presets);
    const bool hasPrevious = previous.isObject() == true;
    const bool isUnchanged = hasPrevious == true && WithoutRevisions(before) == WithoutRevisions(revised);
    const uint32 previousRevision = hasPrevious == true ? GetRevision(before) : 0;
    const uint32 revision = isUnchanged == true ? previousRevision : previousRevision + 1;
    revised.insert("revision"_L1, static_cast<qint64>(revision == 0 ? 1 : revision));
    return revised;
}
} // namespace ludus::editor
