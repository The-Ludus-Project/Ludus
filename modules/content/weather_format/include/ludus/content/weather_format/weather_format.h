#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/byte_order.hpp>

#include <span>
#include <string_view>

/// @file
/// LUWTH cooked climate interchange format (Ludus::WeatherFormat).
///
/// This header is the single contract between the climate cooker
/// (Ludus::ContentWeather) and the runtime weather simulation
/// (Ludus::GameplayWeather). It is owned by neither side and depends only on
/// FoundationBase. See docs/architecture/world-environment.md, "Authored
/// climate and resolved weather", and docs/modules/gameplay/weather.md.
///
/// A blob is little-endian with no padding or alignment. Layout, in order:
///
///   1. One ClimateHeaderRecord (CLIMATE_HEADER_RECORD_BYTES).
///   2. PresetCount WeatherPresetRecord, in strictly ascending Id order.
///   3. ScheduleCount WeatherScheduleRecord, in strictly ascending Id order,
///      whose entry ranges tile the entry table contiguously in that order.
///   4. ScheduleEntryCount WeatherScheduleEntryRecord.
///   5. RegionCount WeatherRegionRecord, in resolution order: descending
///      Priority, then ascending Id.
///
/// The record structs document the exact wire layout. The codecs below
/// serialise them field by field, so host endianness and padding never reach a
/// file. Each *_BYTES width is derived from the field schema. Authoritative
/// channel values are integer codes in the units named by each WeatherChannel,
/// so every target resolves bit-identical weather from the same blob.
namespace ludus::content::weather_format
{
using foundation::int32;
using foundation::int64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Eight-byte file magic, including the trailing NUL.
inline constexpr std::string_view MAGIC{"LUWTH01\0", 8};
/// Version 1: presets, looping schedules, a default schedule and feathered XZ regions.
inline constexpr uint32 FORMAT_VERSION_1 = 1;
/// Current cooked climate layout version.
inline constexpr uint32 FORMAT_VERSION = FORMAT_VERSION_1;

/// Code of the dimensionless fraction 1.0. Fraction codes are fixed point with 16 fractional bits.
inline constexpr int32 FRACTION_ONE = 65536;
/// Temperature codes per degree Celsius (one code is a millidegree).
inline constexpr int32 TEMPERATURE_CODES_PER_DEGREE = 1000;
/// Length and velocity codes per metre (one code is a millimetre, or a millimetre per second).
inline constexpr int32 LENGTH_CODES_PER_METRE = 1000;
/// Fog extinction codes per inverse metre (one code is 1e-6 per metre).
inline constexpr int32 FOG_EXTINCTION_CODES_PER_INVERSE_METRE = 1000000;
/// Precipitation codes per millimetre of liquid water per hour (one code is a micrometre per hour).
inline constexpr int32 PRECIPITATION_CODES_PER_MILLIMETRE_PER_HOUR = 1000;

/// Lowest admitted temperature code (-100 degrees Celsius).
inline constexpr int32 MIN_TEMPERATURE_CODE = -100000;
/// Highest admitted temperature code (+100 degrees Celsius).
inline constexpr int32 MAX_TEMPERATURE_CODE = 100000;
/// Largest admitted magnitude of one wind velocity component code (150 m/s).
inline constexpr int32 MAX_WIND_COMPONENT_CODE = 150000;
/// Largest admitted cloud density multiplier code (8.0).
inline constexpr int32 MAX_CLOUD_DENSITY_SCALE_CODE = 8 * FRACTION_ONE;
/// Largest admitted excess fog extinction code (1 per metre).
inline constexpr int32 MAX_FOG_EXTINCTION_CODE = FOG_EXTINCTION_CODES_PER_INVERSE_METRE;
/// Largest admitted fog scale height code (20 km).
inline constexpr int32 MAX_FOG_SCALE_HEIGHT_CODE = 20000000;
/// Largest admitted precipitation rate code (500 mm/h).
inline constexpr int32 MAX_PRECIPITATION_CODE = 500000;

/// Most presets in one climate.
inline constexpr uint32 MAX_WEATHER_PRESETS = 32;
/// Most schedules in one climate, including the default schedule.
inline constexpr uint32 MAX_WEATHER_SCHEDULES = 16;
/// Most schedule entries across all schedules of one climate.
inline constexpr uint32 MAX_WEATHER_SCHEDULE_ENTRIES = 64;
/// Most feathered regions in one climate, excluding the default region.
inline constexpr uint32 MAX_WEATHER_REGIONS = 8;
/// Lowest admitted session tick rate.
inline constexpr uint32 MIN_TICKS_PER_SECOND = 1;
/// Highest admitted session tick rate.
inline constexpr uint32 MAX_TICKS_PER_SECOND = 1000;
/// Largest admitted magnitude of a World X or Z coordinate code (10,000 km).
inline constexpr int64 MAX_DOMAIN_COORDINATE_CODE = 10000000000;

/// Round value * codesPerUnit to the nearest integer code, ties toward +infinity, and admit it only within
/// [minimumCode, maximumCode]. NaN and infinities fail. The product and the half-code offset are separate
/// binary64 operations, so no target can fuse them and every target produces the same code.
/// @param value Authored or sampled quantity in its natural unit.
/// @param codesPerUnit Positive, exactly representable code scale such as LENGTH_CODES_PER_METRE.
/// @param minimumCode Lowest admitted code.
/// @param maximumCode Highest admitted code; |code| below 2^52.
/// @param outCode Written only on success.
/// @return Whether the rounded code was admitted.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): a value with its named scale and closed code range.
[[nodiscard]] constexpr bool TryQuantizeWeatherCode(foundation::float64 value,
                                                    foundation::float64 codesPerUnit,
                                                    int64 minimumCode,
                                                    int64 maximumCode,
                                                    int64& outCode) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    constexpr foundation::float64 HALF_CODE = 0.5;
    const foundation::float64 scaled = value * codesPerUnit;
    const foundation::float64 lowest = static_cast<foundation::float64>(minimumCode) - HALF_CODE;
    const foundation::float64 highest = static_cast<foundation::float64>(maximumCode) + HALF_CODE;
    // Comparisons with NaN are false, so a NaN or infinite product fails here.
    if ((scaled >= lowest && scaled < highest) == false)
    {
        return false;
    }
    const foundation::float64 shifted = scaled + HALF_CODE;
    int64 code = static_cast<int64>(shifted);
    if (static_cast<foundation::float64>(code) > shifted)
    {
        --code;
    }
    outCode = code;
    return true;
}

/// Authoritative weather channels. Each value is an int32 code in the stated unit.
enum class WeatherChannel : uint8
{
    /// Air temperature at the reference height, in millidegrees Celsius.
    Temperature,
    /// Relative humidity at the reference height, as a fraction code in [0, FRACTION_ONE].
    RelativeHumidity,
    /// Ground wind velocity toward +X (east), in mm/s.
    GroundWindX,
    /// Ground wind velocity toward +Y (up), in mm/s.
    GroundWindY,
    /// Ground wind velocity toward +Z (south), in mm/s.
    GroundWindZ,
    /// Cloud-layer wind velocity toward +X, in mm/s.
    CloudWindX,
    /// Cloud-layer wind velocity toward +Y, in mm/s.
    CloudWindY,
    /// Cloud-layer wind velocity toward +Z, in mm/s.
    CloudWindZ,
    /// Horizontal cloud coverage, as a fraction code in [0, FRACTION_ONE].
    CloudCoverage,
    /// Cloud type weight from stratiform (0) to cumuliform (FRACTION_ONE).
    CloudType,
    /// Cloud density multiplier, as a fraction code in [0, MAX_CLOUD_DENSITY_SCALE_CODE].
    CloudDensityScale,
    /// Excess fog extinction at the height reference plane, in 1e-6 per metre.
    FogExtinction,
    /// Fog scale height above the reference plane, in millimetres.
    FogScaleHeight,
    /// Liquid-water-equivalent precipitation rate, in micrometres per hour.
    Precipitation,
};

/// Number of authoritative weather channels.
inline constexpr usize WEATHER_CHANNEL_COUNT = static_cast<usize>(WeatherChannel::Precipitation) + 1;

/// Channel groups an override can select. Values are bit flags; masks combining them are uint32.
enum class WeatherChannelGroup : uint8
{
    /// WeatherChannel::Temperature.
    Temperature = 1U << 0U,
    /// WeatherChannel::RelativeHumidity.
    Humidity = 1U << 1U,
    /// The three ground wind components.
    GroundWind = 1U << 2U,
    /// The three cloud-layer wind components.
    CloudWind = 1U << 3U,
    /// Coverage, type and density scale.
    Clouds = 1U << 4U,
    /// Fog extinction and scale height.
    Fog = 1U << 5U,
    /// WeatherChannel::Precipitation.
    Precipitation = 1U << 6U,
};

/// Mask selecting every WeatherChannelGroup.
inline constexpr uint32 ALL_WEATHER_CHANNEL_GROUPS = (1U << 7U) - 1U;

/// Return the override group that contains a channel.
[[nodiscard]] constexpr WeatherChannelGroup GetWeatherChannelGroup(WeatherChannel channel) noexcept
{
    switch (channel)
    {
        case WeatherChannel::Temperature:
            return WeatherChannelGroup::Temperature;
        case WeatherChannel::RelativeHumidity:
            return WeatherChannelGroup::Humidity;
        case WeatherChannel::GroundWindX:
        case WeatherChannel::GroundWindY:
        case WeatherChannel::GroundWindZ:
            return WeatherChannelGroup::GroundWind;
        case WeatherChannel::CloudWindX:
        case WeatherChannel::CloudWindY:
        case WeatherChannel::CloudWindZ:
            return WeatherChannelGroup::CloudWind;
        case WeatherChannel::CloudCoverage:
        case WeatherChannel::CloudType:
        case WeatherChannel::CloudDensityScale:
            return WeatherChannelGroup::Clouds;
        case WeatherChannel::FogExtinction:
        case WeatherChannel::FogScaleHeight:
            return WeatherChannelGroup::Fog;
        case WeatherChannel::Precipitation:
            return WeatherChannelGroup::Precipitation;
    }
    return WeatherChannelGroup::Precipitation;
}

/// Whether a code lies in its channel's closed admitted range.
[[nodiscard]] constexpr bool IsWeatherChannelCodeValid(WeatherChannel channel, int32 code) noexcept
{
    switch (channel)
    {
        case WeatherChannel::Temperature:
            return code >= MIN_TEMPERATURE_CODE && code <= MAX_TEMPERATURE_CODE;
        case WeatherChannel::RelativeHumidity:
        case WeatherChannel::CloudCoverage:
        case WeatherChannel::CloudType:
            return code >= 0 && code <= FRACTION_ONE;
        case WeatherChannel::GroundWindX:
        case WeatherChannel::GroundWindY:
        case WeatherChannel::GroundWindZ:
        case WeatherChannel::CloudWindX:
        case WeatherChannel::CloudWindY:
        case WeatherChannel::CloudWindZ:
            return code >= -MAX_WIND_COMPONENT_CODE && code <= MAX_WIND_COMPONENT_CODE;
        case WeatherChannel::CloudDensityScale:
            return code >= 0 && code <= MAX_CLOUD_DENSITY_SCALE_CODE;
        case WeatherChannel::FogExtinction:
            return code >= 0 && code <= MAX_FOG_EXTINCTION_CODE;
        case WeatherChannel::FogScaleHeight:
            return code >= 0 && code <= MAX_FOG_SCALE_HEIGHT_CODE;
        case WeatherChannel::Precipitation:
            return code >= 0 && code <= MAX_PRECIPITATION_CODE;
    }
    return false;
}

/// Authoritative channel codes, indexed by WeatherChannel.
struct WeatherChannelCodes final
{
    /// One code per channel, in WeatherChannel order.
    int32 Values[WEATHER_CHANNEL_COUNT] = {};

    /// Read one channel.
    [[nodiscard]] constexpr int32 Get(WeatherChannel channel) const noexcept
    {
        return Values[static_cast<usize>(channel)];
    }

    /// Write one channel.
    constexpr void Set(WeatherChannel channel, int32 code) noexcept
    {
        Values[static_cast<usize>(channel)] = code;
    }

    /// Whether every code lies in its channel's admitted range.
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        for (usize i = 0; i < WEATHER_CHANNEL_COUNT; ++i)
        {
            if (IsWeatherChannelCodeValid(static_cast<WeatherChannel>(i), Values[i]) == false)
            {
                return false;
            }
        }
        return true;
    }

    /// Codes compare equal channel by channel.
    [[nodiscard]] friend constexpr bool operator==(const WeatherChannelCodes&,
                                                   const WeatherChannelCodes&) noexcept = default;
};

/// Axis-aligned World XZ rectangle in millimetre codes. Minimums are strictly below maximums.
struct WeatherBoundsWorld final
{
    /// Western edge (minimum X).
    int64 MinimumX = 0;
    /// Northern edge (minimum Z; World north is -Z).
    int64 MinimumZ = 0;
    /// Eastern edge (maximum X).
    int64 MaximumX = 0;
    /// Southern edge (maximum Z).
    int64 MaximumZ = 0;

    /// Bounds compare equal edge by edge.
    [[nodiscard]] friend constexpr bool operator==(const WeatherBoundsWorld&,
                                                   const WeatherBoundsWorld&) noexcept = default;
};

/// Whether bounds are nonempty and every edge fits the admitted coordinate range.
[[nodiscard]] constexpr bool IsWeatherBoundsValid(const WeatherBoundsWorld& bounds) noexcept
{
    const auto isCoordinateValid = [](int64 code) noexcept {
        return code >= -MAX_DOMAIN_COORDINATE_CODE && code <= MAX_DOMAIN_COORDINATE_CODE;
    };
    return isCoordinateValid(bounds.MinimumX) == true && isCoordinateValid(bounds.MinimumZ) == true &&
           isCoordinateValid(bounds.MaximumX) == true && isCoordinateValid(bounds.MaximumZ) == true &&
           bounds.MinimumX < bounds.MaximumX && bounds.MinimumZ < bounds.MaximumZ;
}

/// Quantize World XZ bounds given in metres to millimetre codes, admitting only valid nonempty bounds.
/// @param minimumXMetres Western edge.
/// @param minimumZMetres Northern edge (World north is -Z).
/// @param maximumXMetres Eastern edge.
/// @param maximumZMetres Southern edge.
/// @param outBounds Written only on success.
/// @return Whether every edge rounded into range and the bounds are nonempty.
[[nodiscard]] constexpr bool TryQuantizeWeatherBounds(foundation::float64 minimumXMetres,
                                                      foundation::float64 minimumZMetres,
                                                      foundation::float64 maximumXMetres,
                                                      foundation::float64 maximumZMetres,
                                                      WeatherBoundsWorld& outBounds) noexcept
{
    const foundation::float64 scale = LENGTH_CODES_PER_METRE;
    const int64 limit = MAX_DOMAIN_COORDINATE_CODE;
    WeatherBoundsWorld bounds;
    if (TryQuantizeWeatherCode(minimumXMetres, scale, -limit, limit, bounds.MinimumX) == false ||
        TryQuantizeWeatherCode(minimumZMetres, scale, -limit, limit, bounds.MinimumZ) == false ||
        TryQuantizeWeatherCode(maximumXMetres, scale, -limit, limit, bounds.MaximumX) == false ||
        TryQuantizeWeatherCode(maximumZMetres, scale, -limit, limit, bounds.MaximumZ) == false ||
        IsWeatherBoundsValid(bounds) == false)
    {
        return false;
    }
    outBounds = bounds;
    return true;
}

/// Whether the inner bounds lie within the outer bounds, edges included.
[[nodiscard]] constexpr bool IsWeatherBoundsContained(const WeatherBoundsWorld& inner,
                                                      const WeatherBoundsWorld& outer) noexcept
{
    return inner.MinimumX >= outer.MinimumX && inner.MinimumZ >= outer.MinimumZ && inner.MaximumX <= outer.MaximumX &&
           inner.MaximumZ <= outer.MaximumZ;
}

/// Fixed climate header.
struct ClimateHeaderRecord final
{
    /// Must equal MAGIC.
    uint8 Magic[MAGIC.size()] = {};
    /// Layout version; FORMAT_VERSION for blobs this header describes.
    uint32 FormatVersion = FORMAT_VERSION;
    /// Session ticks per second that schedule and override durations assume.
    uint32 TicksPerSecond = 0;
    /// Stable nonzero climate asset ID.
    uint32 ClimateId = 0;
    /// Nonzero authored climate revision.
    uint32 ClimateRevision = 0;
    /// Explicit outer boundary of the climate's World domain.
    WeatherBoundsWorld DomainWorld{};
    /// Number of preset records.
    uint32 PresetCount = 0;
    /// Number of schedule records.
    uint32 ScheduleCount = 0;
    /// Number of schedule entry records across all schedules.
    uint32 ScheduleEntryCount = 0;
    /// Number of region records.
    uint32 RegionCount = 0;
    /// Index of the schedule that drives the default region.
    uint32 DefaultScheduleIndex = 0;
    /// Must be zero.
    uint32 Reserved = 0;
};

/// Serialized width of ClimateHeaderRecord.
inline constexpr usize CLIMATE_HEADER_RECORD_BYTES =
    MAGIC.size() + 4 * sizeof(uint32) + 4 * sizeof(int64) + 6 * sizeof(uint32);

/// One authored weather preset.
struct WeatherPresetRecord final
{
    /// Stable nonzero preset ID referenced by schedules and overrides.
    uint32 Id = 0;
    /// Nonzero authored revision. Saved weather state records it through the climate fingerprint.
    uint32 Revision = 0;
    /// Channel values.
    WeatherChannelCodes Channels{};
};

/// Serialized width of WeatherPresetRecord.
inline constexpr usize WEATHER_PRESET_RECORD_BYTES = 2 * sizeof(uint32) + WEATHER_CHANNEL_COUNT * sizeof(int32);

/// One looping schedule: a contiguous range of entries.
struct WeatherScheduleRecord final
{
    /// Stable nonzero schedule ID.
    uint32 Id = 0;
    /// Index of the schedule's first entry in the entry table.
    uint32 FirstEntry = 0;
    /// Number of entries; at least one.
    uint32 EntryCount = 0;
    /// Entry, relative to FirstEntry, that follows the last entry. Earlier entries play once.
    uint32 LoopStartEntry = 0;
};

/// Serialized width of WeatherScheduleRecord.
inline constexpr usize WEATHER_SCHEDULE_RECORD_BYTES = 4 * sizeof(uint32);

/// One schedule step: hold a preset, then blend toward the next entry.
struct WeatherScheduleEntryRecord final
{
    /// Index of the held preset in the preset table.
    uint32 PresetIndex = 0;
    /// Ticks the preset is held at full weight.
    uint32 HoldTicks = 0;
    /// Ticks spent blending linearly toward the following entry's preset. HoldTicks + BlendTicks is nonzero.
    uint32 BlendTicks = 0;
};

/// Serialized width of WeatherScheduleEntryRecord.
inline constexpr usize WEATHER_SCHEDULE_ENTRY_RECORD_BYTES = 3 * sizeof(uint32);

/// One feathered World XZ region with its own schedule.
struct WeatherRegionRecord final
{
    /// Stable nonzero region ID. Zero names the default region in samples and events.
    uint32 Id = 0;
    /// Higher priorities claim coverage first; ties resolve by ascending Id.
    int32 Priority = 0;
    /// Index of the region's schedule.
    uint32 ScheduleIndex = 0;
    /// Inward feather distance in millimetres. Coverage rises from 0 at the edge to 1 at this depth.
    uint32 FeatherMillimetres = 0;
    /// Outer edge of the region, contained in the climate domain.
    WeatherBoundsWorld BoundsWorld{};
};

/// Serialized width of WeatherRegionRecord.
inline constexpr usize WEATHER_REGION_RECORD_BYTES = 4 * sizeof(uint32) + 4 * sizeof(int64);

/// Largest admitted climate blob.
inline constexpr usize MAX_CLIMATE_BYTES = CLIMATE_HEADER_RECORD_BYTES +
                                           MAX_WEATHER_PRESETS * WEATHER_PRESET_RECORD_BYTES +
                                           MAX_WEATHER_SCHEDULES * WEATHER_SCHEDULE_RECORD_BYTES +
                                           MAX_WEATHER_SCHEDULE_ENTRIES * WEATHER_SCHEDULE_ENTRY_RECORD_BYTES +
                                           MAX_WEATHER_REGIONS * WEATHER_REGION_RECORD_BYTES;

/// Whether a region record precedes another in resolution order.
[[nodiscard]] constexpr bool IsWeatherRegionBefore(const WeatherRegionRecord& left,
                                                   const WeatherRegionRecord& right) noexcept
{
    return left.Priority != right.Priority ? left.Priority > right.Priority : left.Id < right.Id;
}

/// Outcome of a climate format operation. Every failure preserves the caller's outputs.
enum class WeatherFormatStatus : uint8
{
    /// The operation completed.
    Success,
    /// The blob does not start with MAGIC.
    InvalidMagic,
    /// The layout version is not FORMAT_VERSION.
    UnsupportedVersion,
    /// The blob ends before its declared records.
    Truncated,
    /// Bytes follow the last declared record.
    TrailingBytes,
    /// A count exceeds its limit, or a caller-owned output is too small.
    CapacityExceeded,
    /// A header field, ID, revision, duration or channel code is outside its admitted range.
    InvalidValue,
    /// Two presets, schedules or regions share an ID.
    DuplicateId,
    /// Records are not in the canonical order the layout requires.
    NonCanonicalOrder,
    /// A schedule or region refers to a record that does not exist.
    MissingReference,
    /// Domain or region bounds are empty, out of range or outside the domain.
    InvalidBounds,
};

/// Sequential little-endian writer over caller-owned bytes. A failed write stops all later writes.
struct WeatherByteWriter final
{
    /// Destination bytes.
    std::span<uint8> Bytes;
    /// Next byte to write.
    usize Offset = 0;
    /// Whether a write ran past the end of Bytes.
    bool HasFailed = false;

    /// Append one unsigned value.
    template <foundation::UnsignedInteger ValueType>
    constexpr void WriteUnsigned(ValueType value) noexcept
    {
        if (HasFailed == true || Bytes.size() - Offset < sizeof(ValueType) ||
            foundation::TryWriteLittleEndian(value, Bytes.subspan(Offset)) == false)
        {
            HasFailed = true;
            return;
        }
        Offset += sizeof(ValueType);
    }

    /// Append one two's-complement int32.
    constexpr void WriteInt32(int32 value) noexcept
    {
        WriteUnsigned(static_cast<uint32>(value));
    }

    /// Append one two's-complement int64.
    constexpr void WriteInt64(int64 value) noexcept
    {
        WriteUnsigned(static_cast<uint64>(value));
    }
};

/// Sequential little-endian reader over borrowed bytes. A failed read stops all later reads and yields zero.
struct WeatherByteReader final
{
    /// Source bytes; borrowed for the reader's lifetime.
    std::span<const uint8> Bytes;
    /// Next byte to read.
    usize Offset = 0;
    /// Whether a read ran past the end of Bytes.
    bool HasFailed = false;

    /// Read one unsigned value.
    template <foundation::UnsignedInteger ValueType>
    [[nodiscard]] constexpr ValueType ReadUnsigned() noexcept
    {
        ValueType value = 0;
        if (HasFailed == true || Bytes.size() - Offset < sizeof(ValueType) ||
            foundation::TryReadLittleEndian(Bytes.subspan(Offset), value) == false)
        {
            HasFailed = true;
            return 0;
        }
        Offset += sizeof(ValueType);
        return value;
    }

    /// Read one two's-complement int32.
    [[nodiscard]] constexpr int32 ReadInt32() noexcept
    {
        return static_cast<int32>(ReadUnsigned<uint32>());
    }

    /// Read one two's-complement int64.
    [[nodiscard]] constexpr int64 ReadInt64() noexcept
    {
        return static_cast<int64>(ReadUnsigned<uint64>());
    }

    /// Number of bytes not yet read.
    [[nodiscard]] constexpr usize GetRemaining() const noexcept
    {
        return Bytes.size() - Offset;
    }
};

/// Write bounds as minimum X, minimum Z, maximum X, maximum Z.
constexpr void WriteWeatherBounds(WeatherByteWriter& inoutWriter, const WeatherBoundsWorld& bounds) noexcept
{
    inoutWriter.WriteInt64(bounds.MinimumX);
    inoutWriter.WriteInt64(bounds.MinimumZ);
    inoutWriter.WriteInt64(bounds.MaximumX);
    inoutWriter.WriteInt64(bounds.MaximumZ);
}

/// Read bounds written by WriteWeatherBounds.
[[nodiscard]] constexpr WeatherBoundsWorld ReadWeatherBounds(WeatherByteReader& inoutReader) noexcept
{
    WeatherBoundsWorld bounds;
    bounds.MinimumX = inoutReader.ReadInt64();
    bounds.MinimumZ = inoutReader.ReadInt64();
    bounds.MaximumX = inoutReader.ReadInt64();
    bounds.MaximumZ = inoutReader.ReadInt64();
    return bounds;
}

/// Write channel codes in WeatherChannel order.
constexpr void WriteWeatherChannels(WeatherByteWriter& inoutWriter, const WeatherChannelCodes& channels) noexcept
{
    for (const int32 code : channels.Values)
    {
        inoutWriter.WriteInt32(code);
    }
}

/// Read channel codes written by WriteWeatherChannels.
[[nodiscard]] constexpr WeatherChannelCodes ReadWeatherChannels(WeatherByteReader& inoutReader) noexcept
{
    WeatherChannelCodes channels;
    for (int32& code : channels.Values)
    {
        code = inoutReader.ReadInt32();
    }
    return channels;
}

/// Write the header fields in declaration order.
constexpr void WriteClimateHeader(WeatherByteWriter& inoutWriter, const ClimateHeaderRecord& header) noexcept
{
    for (const uint8 byte : header.Magic)
    {
        inoutWriter.WriteUnsigned(byte);
    }
    inoutWriter.WriteUnsigned(header.FormatVersion);
    inoutWriter.WriteUnsigned(header.TicksPerSecond);
    inoutWriter.WriteUnsigned(header.ClimateId);
    inoutWriter.WriteUnsigned(header.ClimateRevision);
    WriteWeatherBounds(inoutWriter, header.DomainWorld);
    inoutWriter.WriteUnsigned(header.PresetCount);
    inoutWriter.WriteUnsigned(header.ScheduleCount);
    inoutWriter.WriteUnsigned(header.ScheduleEntryCount);
    inoutWriter.WriteUnsigned(header.RegionCount);
    inoutWriter.WriteUnsigned(header.DefaultScheduleIndex);
    inoutWriter.WriteUnsigned(header.Reserved);
}

/// Read a header written by WriteClimateHeader. The caller validates its fields.
[[nodiscard]] constexpr ClimateHeaderRecord ReadClimateHeader(WeatherByteReader& inoutReader) noexcept
{
    ClimateHeaderRecord header;
    for (uint8& byte : header.Magic)
    {
        byte = inoutReader.ReadUnsigned<uint8>();
    }
    header.FormatVersion = inoutReader.ReadUnsigned<uint32>();
    header.TicksPerSecond = inoutReader.ReadUnsigned<uint32>();
    header.ClimateId = inoutReader.ReadUnsigned<uint32>();
    header.ClimateRevision = inoutReader.ReadUnsigned<uint32>();
    header.DomainWorld = ReadWeatherBounds(inoutReader);
    header.PresetCount = inoutReader.ReadUnsigned<uint32>();
    header.ScheduleCount = inoutReader.ReadUnsigned<uint32>();
    header.ScheduleEntryCount = inoutReader.ReadUnsigned<uint32>();
    header.RegionCount = inoutReader.ReadUnsigned<uint32>();
    header.DefaultScheduleIndex = inoutReader.ReadUnsigned<uint32>();
    header.Reserved = inoutReader.ReadUnsigned<uint32>();
    return header;
}

/// Write a preset's fields in declaration order.
constexpr void WriteWeatherPreset(WeatherByteWriter& inoutWriter, const WeatherPresetRecord& preset) noexcept
{
    inoutWriter.WriteUnsigned(preset.Id);
    inoutWriter.WriteUnsigned(preset.Revision);
    WriteWeatherChannels(inoutWriter, preset.Channels);
}

/// Read a preset written by WriteWeatherPreset.
[[nodiscard]] constexpr WeatherPresetRecord ReadWeatherPreset(WeatherByteReader& inoutReader) noexcept
{
    WeatherPresetRecord preset;
    preset.Id = inoutReader.ReadUnsigned<uint32>();
    preset.Revision = inoutReader.ReadUnsigned<uint32>();
    preset.Channels = ReadWeatherChannels(inoutReader);
    return preset;
}

/// Write a schedule's fields in declaration order.
constexpr void WriteWeatherSchedule(WeatherByteWriter& inoutWriter, const WeatherScheduleRecord& schedule) noexcept
{
    inoutWriter.WriteUnsigned(schedule.Id);
    inoutWriter.WriteUnsigned(schedule.FirstEntry);
    inoutWriter.WriteUnsigned(schedule.EntryCount);
    inoutWriter.WriteUnsigned(schedule.LoopStartEntry);
}

/// Read a schedule written by WriteWeatherSchedule.
[[nodiscard]] constexpr WeatherScheduleRecord ReadWeatherSchedule(WeatherByteReader& inoutReader) noexcept
{
    WeatherScheduleRecord schedule;
    schedule.Id = inoutReader.ReadUnsigned<uint32>();
    schedule.FirstEntry = inoutReader.ReadUnsigned<uint32>();
    schedule.EntryCount = inoutReader.ReadUnsigned<uint32>();
    schedule.LoopStartEntry = inoutReader.ReadUnsigned<uint32>();
    return schedule;
}

/// Write a schedule entry's fields in declaration order.
constexpr void WriteWeatherScheduleEntry(WeatherByteWriter& inoutWriter,
                                         const WeatherScheduleEntryRecord& entry) noexcept
{
    inoutWriter.WriteUnsigned(entry.PresetIndex);
    inoutWriter.WriteUnsigned(entry.HoldTicks);
    inoutWriter.WriteUnsigned(entry.BlendTicks);
}

/// Read a schedule entry written by WriteWeatherScheduleEntry.
[[nodiscard]] constexpr WeatherScheduleEntryRecord ReadWeatherScheduleEntry(WeatherByteReader& inoutReader) noexcept
{
    WeatherScheduleEntryRecord entry;
    entry.PresetIndex = inoutReader.ReadUnsigned<uint32>();
    entry.HoldTicks = inoutReader.ReadUnsigned<uint32>();
    entry.BlendTicks = inoutReader.ReadUnsigned<uint32>();
    return entry;
}

/// Write a region's fields in declaration order.
constexpr void WriteWeatherRegion(WeatherByteWriter& inoutWriter, const WeatherRegionRecord& region) noexcept
{
    inoutWriter.WriteUnsigned(region.Id);
    inoutWriter.WriteInt32(region.Priority);
    inoutWriter.WriteUnsigned(region.ScheduleIndex);
    inoutWriter.WriteUnsigned(region.FeatherMillimetres);
    WriteWeatherBounds(inoutWriter, region.BoundsWorld);
}

/// Read a region written by WriteWeatherRegion.
[[nodiscard]] constexpr WeatherRegionRecord ReadWeatherRegion(WeatherByteReader& inoutReader) noexcept
{
    WeatherRegionRecord region;
    region.Id = inoutReader.ReadUnsigned<uint32>();
    region.Priority = inoutReader.ReadInt32();
    region.ScheduleIndex = inoutReader.ReadUnsigned<uint32>();
    region.FeatherMillimetres = inoutReader.ReadUnsigned<uint32>();
    region.BoundsWorld = ReadWeatherBounds(inoutReader);
    return region;
}

/// Whether a header's magic matches MAGIC.
[[nodiscard]] constexpr bool HasClimateMagic(const ClimateHeaderRecord& header) noexcept
{
    for (usize i = 0; i < MAGIC.size(); ++i)
    {
        if (header.Magic[i] != static_cast<uint8>(MAGIC[i]))
        {
            return false;
        }
    }
    return true;
}

/// Borrowed record tables of one climate, in serialized order.
struct ClimateRecordsView final
{
    /// Header; counts must equal the table extents.
    const ClimateHeaderRecord* HeaderOrNull = nullptr;
    /// Presets in ascending Id order.
    std::span<const WeatherPresetRecord> Presets;
    /// Schedules in ascending Id order.
    std::span<const WeatherScheduleRecord> Schedules;
    /// Entries of every schedule, contiguous in schedule order.
    std::span<const WeatherScheduleEntryRecord> Entries;
    /// Regions in resolution order.
    std::span<const WeatherRegionRecord> Regions;
};

/// Validate every semantic rule of a climate's records: limits, IDs, revisions, channel ranges,
/// canonical order, entry tiling, references, durations and bounds. The cooker validates before
/// writing and the runtime validates after reading, so both admit exactly the same climates.
[[nodiscard]] constexpr WeatherFormatStatus ValidateClimateRecords(const ClimateRecordsView& records) noexcept
{
    if (records.HeaderOrNull == nullptr)
    {
        return WeatherFormatStatus::InvalidValue;
    }
    const ClimateHeaderRecord& header = *records.HeaderOrNull;
    if (HasClimateMagic(header) == false)
    {
        return WeatherFormatStatus::InvalidMagic;
    }
    if (header.FormatVersion != FORMAT_VERSION)
    {
        return WeatherFormatStatus::UnsupportedVersion;
    }
    if (header.PresetCount > MAX_WEATHER_PRESETS || header.ScheduleCount > MAX_WEATHER_SCHEDULES ||
        header.ScheduleEntryCount > MAX_WEATHER_SCHEDULE_ENTRIES || header.RegionCount > MAX_WEATHER_REGIONS)
    {
        return WeatherFormatStatus::CapacityExceeded;
    }
    if (records.Presets.size() != header.PresetCount || records.Schedules.size() != header.ScheduleCount ||
        records.Entries.size() != header.ScheduleEntryCount || records.Regions.size() != header.RegionCount ||
        header.PresetCount == 0 || header.ScheduleCount == 0 || header.Reserved != 0 || header.ClimateId == 0 ||
        header.ClimateRevision == 0 || header.TicksPerSecond < MIN_TICKS_PER_SECOND ||
        header.TicksPerSecond > MAX_TICKS_PER_SECOND || header.DefaultScheduleIndex >= header.ScheduleCount)
    {
        return WeatherFormatStatus::InvalidValue;
    }
    if (IsWeatherBoundsValid(header.DomainWorld) == false)
    {
        return WeatherFormatStatus::InvalidBounds;
    }
    for (usize i = 0; i < records.Presets.size(); ++i)
    {
        const WeatherPresetRecord& preset = records.Presets[i];
        if (preset.Id == 0 || preset.Revision == 0 || preset.Channels.IsValid() == false)
        {
            return WeatherFormatStatus::InvalidValue;
        }
        if (i > 0 && records.Presets[i - 1].Id >= preset.Id)
        {
            return records.Presets[i - 1].Id == preset.Id ? WeatherFormatStatus::DuplicateId
                                                          : WeatherFormatStatus::NonCanonicalOrder;
        }
    }
    uint32 nextEntry = 0;
    for (usize i = 0; i < records.Schedules.size(); ++i)
    {
        const WeatherScheduleRecord& schedule = records.Schedules[i];
        if (schedule.Id == 0 || schedule.EntryCount == 0 || schedule.LoopStartEntry >= schedule.EntryCount)
        {
            return WeatherFormatStatus::InvalidValue;
        }
        if (i > 0 && records.Schedules[i - 1].Id >= schedule.Id)
        {
            return records.Schedules[i - 1].Id == schedule.Id ? WeatherFormatStatus::DuplicateId
                                                              : WeatherFormatStatus::NonCanonicalOrder;
        }
        if (schedule.FirstEntry != nextEntry || schedule.EntryCount > header.ScheduleEntryCount - nextEntry)
        {
            return WeatherFormatStatus::NonCanonicalOrder;
        }
        nextEntry += schedule.EntryCount;
    }
    if (nextEntry != header.ScheduleEntryCount)
    {
        return WeatherFormatStatus::NonCanonicalOrder;
    }
    for (const WeatherScheduleEntryRecord& entry : records.Entries)
    {
        if (entry.PresetIndex >= header.PresetCount)
        {
            return WeatherFormatStatus::MissingReference;
        }
        if (entry.HoldTicks == 0 && entry.BlendTicks == 0)
        {
            return WeatherFormatStatus::InvalidValue;
        }
    }
    for (usize i = 0; i < records.Regions.size(); ++i)
    {
        const WeatherRegionRecord& region = records.Regions[i];
        if (region.Id == 0)
        {
            return WeatherFormatStatus::InvalidValue;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (records.Regions[j].Id == region.Id)
            {
                return WeatherFormatStatus::DuplicateId;
            }
        }
        if (i > 0 && IsWeatherRegionBefore(records.Regions[i - 1], region) == false)
        {
            return WeatherFormatStatus::NonCanonicalOrder;
        }
        if (region.ScheduleIndex >= header.ScheduleCount)
        {
            return WeatherFormatStatus::MissingReference;
        }
        if (IsWeatherBoundsValid(region.BoundsWorld) == false ||
            IsWeatherBoundsContained(region.BoundsWorld, header.DomainWorld) == false)
        {
            return WeatherFormatStatus::InvalidBounds;
        }
    }
    return WeatherFormatStatus::Success;
}

/// Serialized byte count of a climate with the header's record counts.
[[nodiscard]] constexpr usize GetClimateBytes(const ClimateHeaderRecord& header) noexcept
{
    return CLIMATE_HEADER_RECORD_BYTES + usize{header.PresetCount} * WEATHER_PRESET_RECORD_BYTES +
           usize{header.ScheduleCount} * WEATHER_SCHEDULE_RECORD_BYTES +
           usize{header.ScheduleEntryCount} * WEATHER_SCHEDULE_ENTRY_RECORD_BYTES +
           usize{header.RegionCount} * WEATHER_REGION_RECORD_BYTES;
}
} // namespace ludus::content::weather_format
