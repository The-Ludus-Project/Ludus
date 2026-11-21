#include <ludus/gameplay/weather/climate.h>

#include <span>

namespace ludus::gameplay::weather
{
namespace format = weather_format;

bool Climate::TryFindPresetIndex(uint32 id, uint32& outIndex) const noexcept
{
    // Presets are sorted by ID, so a binary search finds the record.
    usize low = 0;
    usize high = mPresets.GetSize();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (mPresets[middle].Id < id)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == mPresets.GetSize() || mPresets[low].Id != id)
    {
        return false;
    }
    outIndex = static_cast<uint32>(low);
    return true;
}

format::WeatherFormatStatus TryLoadClimate(foundation::BorrowedView<const uint8> bytes, Climate& outClimate) noexcept
{
    if (bytes.IsValid() == false)
    {
        return format::WeatherFormatStatus::InvalidValue;
    }
    if (bytes.GetSize() > format::MAX_CLIMATE_BYTES)
    {
        return format::WeatherFormatStatus::CapacityExceeded;
    }
    const std::span<const uint8> source(bytes.GetDataOrNull(), bytes.GetSize());
    format::WeatherByteReader reader{ .Bytes = source };
    Climate climate;
    climate.mHeader = format::ReadClimateHeader(reader);
    if (reader.HasFailed == true)
    {
        return format::WeatherFormatStatus::Truncated;
    }
    const format::ClimateHeaderRecord& header = climate.mHeader;
    if (format::HasClimateMagic(header) == false)
    {
        return format::WeatherFormatStatus::InvalidMagic;
    }
    if (header.FormatVersion != format::FORMAT_VERSION)
    {
        return format::WeatherFormatStatus::UnsupportedVersion;
    }
    if (header.PresetCount > format::MAX_WEATHER_PRESETS || header.ScheduleCount > format::MAX_WEATHER_SCHEDULES ||
        header.ScheduleEntryCount > format::MAX_WEATHER_SCHEDULE_ENTRIES ||
        header.RegionCount > format::MAX_WEATHER_REGIONS)
    {
        return format::WeatherFormatStatus::CapacityExceeded;
    }
    const usize expectedBytes = format::GetClimateBytes(header);
    if (bytes.GetSize() < expectedBytes)
    {
        return format::WeatherFormatStatus::Truncated;
    }
    if (bytes.GetSize() > expectedBytes)
    {
        return format::WeatherFormatStatus::TrailingBytes;
    }
    for (uint32 i = 0; i < header.PresetCount; ++i)
    {
        LUDUS_REQUIRE(climate.mPresets.TryAdd(format::ReadWeatherPreset(reader)) == true);
    }
    for (uint32 i = 0; i < header.ScheduleCount; ++i)
    {
        LUDUS_REQUIRE(climate.mSchedules.TryAdd(format::ReadWeatherSchedule(reader)) == true);
    }
    for (uint32 i = 0; i < header.ScheduleEntryCount; ++i)
    {
        LUDUS_REQUIRE(climate.mEntries.TryAdd(format::ReadWeatherScheduleEntry(reader)) == true);
    }
    for (uint32 i = 0; i < header.RegionCount; ++i)
    {
        LUDUS_REQUIRE(climate.mRegions.TryAdd(format::ReadWeatherRegion(reader)) == true);
    }
    LUDUS_REQUIRE(reader.HasFailed == false && reader.GetRemaining() == 0);

    const format::ClimateRecordsView records
    {
        .HeaderOrNull = &climate.mHeader,
        .Presets = std::span<const format::WeatherPresetRecord>(climate.mPresets.GetData(), climate.mPresets.GetSize()),
        .Schedules =
            std::span<const format::WeatherScheduleRecord>(climate.mSchedules.GetData(), climate.mSchedules.GetSize()),
        .Entries =
            std::span<const format::WeatherScheduleEntryRecord>(climate.mEntries.GetData(), climate.mEntries.GetSize()),
        .Regions = std::span<const format::WeatherRegionRecord>(climate.mRegions.GetData(), climate.mRegions.GetSize()),
    };
    const format::WeatherFormatStatus status = format::ValidateClimateRecords(records);
    if (status != format::WeatherFormatStatus::Success)
    {
        return status;
    }
    climate.mFingerprint = foundation::StableFingerprint128(source);
    outClimate = foundation::Move(climate);
    return format::WeatherFormatStatus::Success;
}
} // namespace ludus::gameplay::weather
