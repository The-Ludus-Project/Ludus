#include <ludus/content/weather/climate_cook.h>

#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <span>

namespace ludus::content::weather
{
namespace
{
namespace format = weather_format;
using format::WeatherChannel;
using foundation::FixedArray;
using foundation::int64;
using foundation::uint64;
using foundation::usize;

using PresetRecords = FixedArray<format::WeatherPresetRecord, format::MAX_WEATHER_PRESETS>;
using ScheduleRecords = FixedArray<format::WeatherScheduleRecord, format::MAX_WEATHER_SCHEDULES>;
using EntryRecords = FixedArray<format::WeatherScheduleEntryRecord, format::MAX_WEATHER_SCHEDULE_ENTRIES>;
using RegionRecords = FixedArray<format::WeatherRegionRecord, format::MAX_WEATHER_REGIONS>;

/// Largest feather distance a region record can carry, in millimetre codes.
constexpr int64 MAX_FEATHER_CODE = 0xffffffff;
/// Lowest and highest int32 values; channel ranges then narrow the admitted code.
constexpr int64 LOWEST_INT32_CODE = -(int64{1} << 31);
constexpr int64 HIGHEST_INT32_CODE = (int64{1} << 31) - 1;

ClimateCookResult
MakeFailure(ClimateCookStatus status, ClimateCookSubject subject, uint32 subjectId = 0, uint32 detail = 0) noexcept
{
    return { .Status = status, .Subject = subject, .SubjectId = subjectId, .Detail = detail };
}

bool TryQuantizeChannel(float64 value, float64 codesPerUnit, WeatherChannel channel, int32& outCode) noexcept
{
    int64 code = 0;
    if (format::TryQuantizeWeatherCode(value, codesPerUnit, LOWEST_INT32_CODE, HIGHEST_INT32_CODE, code) == false ||
        format::IsWeatherChannelCodeValid(channel, static_cast<int32>(code)) == false)
    {
        return false;
    }
    outCode = static_cast<int32>(code);
    return true;
}

/// Quantize every channel of a preset, reporting the first failing channel in outFailedChannel.
bool TryQuantizePreset(const WeatherPresetSource& source,
                       format::WeatherChannelCodes& outChannels,
                       WeatherChannel& outFailedChannel) noexcept
{
    const float64 fraction = format::FRACTION_ONE;
    const float64 length = format::LENGTH_CODES_PER_METRE;
    struct ChannelValue final
    {
        WeatherChannel Channel;
        float64 Value;
        float64 CodesPerUnit;
    };
    const foundation::StaticArray<ChannelValue, format::WEATHER_CHANNEL_COUNT> values{
        ChannelValue{WeatherChannel::Temperature, source.TemperatureCelsius, format::TEMPERATURE_CODES_PER_DEGREE},
        ChannelValue{WeatherChannel::RelativeHumidity, source.RelativeHumidity, fraction},
        ChannelValue{WeatherChannel::GroundWindX, source.GroundWindWorld.X, length},
        ChannelValue{WeatherChannel::GroundWindY, source.GroundWindWorld.Y, length},
        ChannelValue{WeatherChannel::GroundWindZ, source.GroundWindWorld.Z, length},
        ChannelValue{WeatherChannel::CloudWindX, source.CloudWindWorld.X, length},
        ChannelValue{WeatherChannel::CloudWindY, source.CloudWindWorld.Y, length},
        ChannelValue{WeatherChannel::CloudWindZ, source.CloudWindWorld.Z, length},
        ChannelValue{WeatherChannel::CloudCoverage, source.CloudCoverage, fraction},
        ChannelValue{WeatherChannel::CloudType, source.CloudType, fraction},
        ChannelValue{WeatherChannel::CloudDensityScale, source.CloudDensityScale, fraction},
        ChannelValue{WeatherChannel::FogExtinction,
                     source.FogExtinctionPerMetre,
                     format::FOG_EXTINCTION_CODES_PER_INVERSE_METRE},
        ChannelValue{WeatherChannel::FogScaleHeight, source.FogScaleHeightMetres, length},
        ChannelValue{WeatherChannel::Precipitation,
                     source.PrecipitationMillimetresPerHour,
                     format::PRECIPITATION_CODES_PER_MILLIMETRE_PER_HOUR},
    };
    format::WeatherChannelCodes channels;
    for (const ChannelValue& entry : values)
    {
        int32 code = 0;
        if (TryQuantizeChannel(entry.Value, entry.CodesPerUnit, entry.Channel, code) == false)
        {
            outFailedChannel = entry.Channel;
            return false;
        }
        channels.Set(entry.Channel, code);
    }
    outChannels = channels;
    return true;
}

bool TryQuantizeBounds(const WeatherBoundsMetres& source, format::WeatherBoundsWorld& outBounds) noexcept
{
    return format::TryQuantizeWeatherBounds(source.MinimumX,
                                            source.MinimumZ,
                                            source.MaximumX,
                                            source.MaximumZ,
                                            outBounds);
}

bool TryFindPresetIndex(const PresetRecords& presets, uint32 id, uint32& outIndex) noexcept
{
    for (usize i = 0; i < presets.GetSize(); ++i)
    {
        if (presets[i].Id == id)
        {
            outIndex = static_cast<uint32>(i);
            return true;
        }
    }
    return false;
}

bool TryFindScheduleIndex(const ScheduleRecords& schedules, uint32 id, uint32& outIndex) noexcept
{
    for (usize i = 0; i < schedules.GetSize(); ++i)
    {
        if (schedules[i].Id == id)
        {
            outIndex = static_cast<uint32>(i);
            return true;
        }
    }
    return false;
}

ClimateCookResult CookPresets(const ClimateSource& source, PresetRecords& outPresets) noexcept
{
    for (const WeatherPresetSource& preset : source.Presets)
    {
        if (preset.Id == 0 || preset.Revision == 0)
        {
            return MakeFailure(ClimateCookStatus::InvalidArgument, ClimateCookSubject::Preset, preset.Id);
        }
        for (const format::WeatherPresetRecord& existing : outPresets)
        {
            if (existing.Id == preset.Id)
            {
                return MakeFailure(ClimateCookStatus::DuplicateId, ClimateCookSubject::Preset, preset.Id);
            }
        }
        format::WeatherPresetRecord record{ .Id = preset.Id, .Revision = preset.Revision };
        WeatherChannel failedChannel = WeatherChannel::Temperature;
        if (TryQuantizePreset(preset, record.Channels, failedChannel) == false)
        {
            return MakeFailure(ClimateCookStatus::ValueOutOfRange,
                               ClimateCookSubject::Preset,
                               preset.Id,
                               static_cast<uint32>(failedChannel));
        }
        LUDUS_REQUIRE(outPresets.TryAdd(record) == true);
    }
    foundation::Sort(outPresets.GetData(),
                     outPresets.GetSize(),
                     [](const format::WeatherPresetRecord& left, const format::WeatherPresetRecord& right) {
                         return left.Id < right.Id;
                     });
    return {};
}

ClimateCookResult CookSchedules(const ClimateSource& source,
                                const PresetRecords& presets,
                                ScheduleRecords& outSchedules,
                                EntryRecords& outEntries) noexcept
{
    // Order schedule sources by ID first, so the entry table tiles in canonical schedule order.
    FixedArray<const WeatherScheduleSource*, format::MAX_WEATHER_SCHEDULES> ordered;
    for (const WeatherScheduleSource& schedule : source.Schedules)
    {
        if (schedule.Id == 0 || schedule.Entries.IsValid() == false || schedule.Entries.IsEmpty() == true ||
            schedule.LoopStartEntry >= schedule.Entries.GetSize())
        {
            return MakeFailure(ClimateCookStatus::InvalidArgument, ClimateCookSubject::Schedule, schedule.Id);
        }
        for (const WeatherScheduleSource* existing : ordered)
        {
            if (existing->Id == schedule.Id)
            {
                return MakeFailure(ClimateCookStatus::DuplicateId, ClimateCookSubject::Schedule, schedule.Id);
            }
        }
        LUDUS_REQUIRE(ordered.TryAdd(&schedule) == true);
    }
    foundation::Sort(
        ordered.GetData(),
        ordered.GetSize(),
        [](const WeatherScheduleSource* left, const WeatherScheduleSource* right) { return left->Id < right->Id; });
    for (const WeatherScheduleSource* schedule : ordered)
    {
        if (schedule->Entries.GetSize() > outEntries.GetCapacity() - outEntries.GetSize())
        {
            return MakeFailure(ClimateCookStatus::CapacityExceeded, ClimateCookSubject::Schedule, schedule->Id);
        }
        const format::WeatherScheduleRecord record
        {
            .Id = schedule->Id,
            .FirstEntry = static_cast<uint32>(outEntries.GetSize()),
            .EntryCount = static_cast<uint32>(schedule->Entries.GetSize()),
            .LoopStartEntry = schedule->LoopStartEntry,
        };
        for (usize i = 0; i < schedule->Entries.GetSize(); ++i)
        {
            const WeatherScheduleEntrySource& entry = schedule->Entries[i];
            const uint32 entryIndex = static_cast<uint32>(i);
            format::WeatherScheduleEntryRecord entryRecord
            {
                .HoldTicks = entry.HoldTicks,
                .BlendTicks = entry.BlendTicks,
            };
            if (TryFindPresetIndex(presets, entry.PresetId, entryRecord.PresetIndex) == false)
            {
                return MakeFailure(ClimateCookStatus::MissingReference,
                                   ClimateCookSubject::ScheduleEntry,
                                   schedule->Id,
                                   entryIndex);
            }
            if (entry.HoldTicks == 0 && entry.BlendTicks == 0)
            {
                return MakeFailure(ClimateCookStatus::InvalidArgument,
                                   ClimateCookSubject::ScheduleEntry,
                                   schedule->Id,
                                   entryIndex);
            }
            LUDUS_REQUIRE(outEntries.TryAdd(entryRecord) == true);
        }
        LUDUS_REQUIRE(outSchedules.TryAdd(record) == true);
    }
    return {};
}

ClimateCookResult CookRegions(const ClimateSource& source,
                              const ScheduleRecords& schedules,
                              const format::WeatherBoundsWorld& domain,
                              RegionRecords& outRegions) noexcept
{
    for (const WeatherRegionSource& region : source.Regions)
    {
        if (region.Id == 0)
        {
            return MakeFailure(ClimateCookStatus::InvalidArgument, ClimateCookSubject::Region, region.Id);
        }
        for (const format::WeatherRegionRecord& existing : outRegions)
        {
            if (existing.Id == region.Id)
            {
                return MakeFailure(ClimateCookStatus::DuplicateId, ClimateCookSubject::Region, region.Id);
            }
        }
        format::WeatherRegionRecord record{ .Id = region.Id, .Priority = region.Priority };
        if (TryFindScheduleIndex(schedules, region.ScheduleId, record.ScheduleIndex) == false)
        {
            return MakeFailure(ClimateCookStatus::MissingReference, ClimateCookSubject::Region, region.Id);
        }
        if (TryQuantizeBounds(region.BoundsWorld, record.BoundsWorld) == false ||
            format::IsWeatherBoundsContained(record.BoundsWorld, domain) == false)
        {
            return MakeFailure(ClimateCookStatus::InvalidBounds, ClimateCookSubject::Region, region.Id);
        }
        int64 feather = 0;
        if (format::TryQuantizeWeatherCode(region.FeatherMetres,
                                           format::LENGTH_CODES_PER_METRE,
                                           0,
                                           MAX_FEATHER_CODE,
                                           feather) == false)
        {
            return MakeFailure(ClimateCookStatus::ValueOutOfRange, ClimateCookSubject::Region, region.Id);
        }
        record.FeatherMillimetres = static_cast<uint32>(feather);
        LUDUS_REQUIRE(outRegions.TryAdd(record) == true);
    }
    foundation::Sort(outRegions.GetData(), outRegions.GetSize(), format::IsWeatherRegionBefore);
    return {};
}

ClimateCookStatus MapFormatStatus(format::WeatherFormatStatus status) noexcept
{
    switch (status)
    {
        case format::WeatherFormatStatus::Success:
            return ClimateCookStatus::Success;
        case format::WeatherFormatStatus::CapacityExceeded:
            return ClimateCookStatus::CapacityExceeded;
        case format::WeatherFormatStatus::DuplicateId:
            return ClimateCookStatus::DuplicateId;
        case format::WeatherFormatStatus::MissingReference:
            return ClimateCookStatus::MissingReference;
        case format::WeatherFormatStatus::InvalidBounds:
            return ClimateCookStatus::InvalidBounds;
        case format::WeatherFormatStatus::InvalidMagic:
        case format::WeatherFormatStatus::UnsupportedVersion:
        case format::WeatherFormatStatus::Truncated:
        case format::WeatherFormatStatus::TrailingBytes:
        case format::WeatherFormatStatus::InvalidValue:
        case format::WeatherFormatStatus::NonCanonicalOrder:
            return ClimateCookStatus::InvalidArgument;
    }
    return ClimateCookStatus::InvalidArgument;
}
} // namespace

ClimateCookResult TryCookClimate(const ClimateSource& source, foundation::Array<uint8>& outBytes) noexcept
{
    if (source.Presets.IsValid() == false || source.Schedules.IsValid() == false || source.Regions.IsValid() == false ||
        source.Id == 0 || source.Revision == 0 || source.TicksPerSecond < format::MIN_TICKS_PER_SECOND ||
        source.TicksPerSecond > format::MAX_TICKS_PER_SECOND || source.Presets.IsEmpty() == true ||
        source.Schedules.IsEmpty() == true)
    {
        return MakeFailure(ClimateCookStatus::InvalidArgument, ClimateCookSubject::Climate);
    }
    if (source.Presets.GetSize() > format::MAX_WEATHER_PRESETS ||
        source.Schedules.GetSize() > format::MAX_WEATHER_SCHEDULES ||
        source.Regions.GetSize() > format::MAX_WEATHER_REGIONS)
    {
        return MakeFailure(ClimateCookStatus::CapacityExceeded, ClimateCookSubject::Climate);
    }

    format::ClimateHeaderRecord header
    {
        .TicksPerSecond = source.TicksPerSecond,
        .ClimateId = source.Id,
        .ClimateRevision = source.Revision,
    };
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        header.Magic[i] = static_cast<uint8>(format::MAGIC[i]);
    }
    if (TryQuantizeBounds(source.DomainWorld, header.DomainWorld) == false)
    {
        return MakeFailure(ClimateCookStatus::InvalidBounds, ClimateCookSubject::Climate);
    }

    PresetRecords presets;
    ScheduleRecords schedules;
    EntryRecords entries;
    RegionRecords regions;
    ClimateCookResult result = CookPresets(source, presets);
    if (result.Status != ClimateCookStatus::Success)
    {
        return result;
    }
    result = CookSchedules(source, presets, schedules, entries);
    if (result.Status != ClimateCookStatus::Success)
    {
        return result;
    }
    result = CookRegions(source, schedules, header.DomainWorld, regions);
    if (result.Status != ClimateCookStatus::Success)
    {
        return result;
    }
    if (TryFindScheduleIndex(schedules, source.DefaultScheduleId, header.DefaultScheduleIndex) == false)
    {
        return MakeFailure(ClimateCookStatus::MissingReference, ClimateCookSubject::Climate);
    }
    header.PresetCount = static_cast<uint32>(presets.GetSize());
    header.ScheduleCount = static_cast<uint32>(schedules.GetSize());
    header.ScheduleEntryCount = static_cast<uint32>(entries.GetSize());
    header.RegionCount = static_cast<uint32>(regions.GetSize());

    const format::ClimateRecordsView records
    {
        .HeaderOrNull = &header,
        .Presets = std::span<const format::WeatherPresetRecord>(presets.GetData(), presets.GetSize()),
        .Schedules = std::span<const format::WeatherScheduleRecord>(schedules.GetData(), schedules.GetSize()),
        .Entries = std::span<const format::WeatherScheduleEntryRecord>(entries.GetData(), entries.GetSize()),
        .Regions = std::span<const format::WeatherRegionRecord>(regions.GetData(), regions.GetSize()),
    };
    const format::WeatherFormatStatus validation = format::ValidateClimateRecords(records);
    if (validation != format::WeatherFormatStatus::Success)
    {
        return MakeFailure(MapFormatStatus(validation), ClimateCookSubject::Climate);
    }

    foundation::Array<uint8> bytes;
    if (bytes.TryResize(format::GetClimateBytes(header)) == false)
    {
        return MakeFailure(ClimateCookStatus::AllocationFailed, ClimateCookSubject::Climate);
    }
    format::WeatherByteWriter writer{ .Bytes = std::span<uint8>(bytes.GetData(), bytes.GetSize()) };
    format::WriteClimateHeader(writer, header);
    for (const format::WeatherPresetRecord& preset : presets)
    {
        format::WriteWeatherPreset(writer, preset);
    }
    for (const format::WeatherScheduleRecord& schedule : schedules)
    {
        format::WriteWeatherSchedule(writer, schedule);
    }
    for (const format::WeatherScheduleEntryRecord& entry : entries)
    {
        format::WriteWeatherScheduleEntry(writer, entry);
    }
    for (const format::WeatherRegionRecord& region : regions)
    {
        format::WriteWeatherRegion(writer, region);
    }
    LUDUS_REQUIRE(writer.HasFailed == false && writer.Offset == bytes.GetSize());
    outBytes = foundation::Move(bytes);
    return {};
}
} // namespace ludus::content::weather
