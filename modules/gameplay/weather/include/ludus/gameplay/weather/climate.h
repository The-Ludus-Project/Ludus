#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/weather_format/weather_format.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/hash/hash.hpp>

/// @file
/// Immutable runtime climate decoded from one LUWTH blob (Ludus::GameplayWeather).
/// See docs/modules/gameplay/weather.md.
namespace ludus::gameplay::weather
{
namespace weather_format = content::weather_format;
using foundation::float64;
using foundation::int32;
using foundation::int64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Validated climate records and the fingerprint of the exact blob they came from.
///
/// A Climate is a bounded value type: it owns copies of every record and needs no allocation, so it can
/// be copied into a simulation and outlive the blob it was loaded from. A default Climate is empty.
class Climate final
{
public:
    /// Whether no climate has been loaded.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mPresets.IsEmpty();
    }

    /// Header of the loaded blob. Meaningless for an empty climate.
    [[nodiscard]] LUDUS_INLINE const weather_format::ClimateHeaderRecord& GetHeader() const noexcept
    {
        return mHeader;
    }

    /// Presets in ascending ID order.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const weather_format::WeatherPresetRecord>
    GetPresets() const noexcept
    {
        return {mPresets.GetData(), mPresets.GetSize()};
    }

    /// Schedules in ascending ID order.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const weather_format::WeatherScheduleRecord>
    GetSchedules() const noexcept
    {
        return {mSchedules.GetData(), mSchedules.GetSize()};
    }

    /// Entries of every schedule, contiguous in schedule order.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const weather_format::WeatherScheduleEntryRecord>
    GetScheduleEntries() const noexcept
    {
        return {mEntries.GetData(), mEntries.GetSize()};
    }

    /// Regions in resolution order: descending priority, then ascending ID.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const weather_format::WeatherRegionRecord>
    GetRegions() const noexcept
    {
        return {mRegions.GetData(), mRegions.GetSize()};
    }

    /// StableFingerprint128 of the exact blob. Saved weather records it to detect any climate or preset change.
    [[nodiscard]] LUDUS_INLINE foundation::Fingerprint128 GetFingerprint() const noexcept
    {
        return mFingerprint;
    }

    /// Find a preset by its stable ID.
    /// @param id Preset ID.
    /// @param outIndex Index into GetPresets(); written only when found.
    /// @return Whether the climate has the preset.
    [[nodiscard]] bool TryFindPresetIndex(uint32 id, uint32& outIndex) const noexcept;

private:
    friend weather_format::WeatherFormatStatus TryLoadClimate(foundation::BorrowedView<const uint8> bytes,
                                                              Climate& outClimate) noexcept;

    weather_format::ClimateHeaderRecord mHeader{};
    foundation::FixedArray<weather_format::WeatherPresetRecord, weather_format::MAX_WEATHER_PRESETS> mPresets;
    foundation::FixedArray<weather_format::WeatherScheduleRecord, weather_format::MAX_WEATHER_SCHEDULES> mSchedules;
    foundation::FixedArray<weather_format::WeatherScheduleEntryRecord, weather_format::MAX_WEATHER_SCHEDULE_ENTRIES>
        mEntries;
    foundation::FixedArray<weather_format::WeatherRegionRecord, weather_format::MAX_WEATHER_REGIONS> mRegions;
    foundation::Fingerprint128 mFingerprint{};
};

/// Decode and validate one LUWTH blob with the same rules the cooker applies.
/// @param bytes Exact blob, borrowed for the call only. Truncation and trailing bytes both fail.
/// @param outClimate Replaced only on Success.
/// @return Success, or the first format rule the blob violates.
[[nodiscard]] weather_format::WeatherFormatStatus TryLoadClimate(foundation::BorrowedView<const uint8> bytes,
                                                                 Climate& outClimate) noexcept;
} // namespace ludus::gameplay::weather
