// LUWTH format contract: frozen widths, quantization rounding, channel ranges and record codecs.
#include <ludus/content/weather_format/weather_format.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace format = ludus::content::weather_format;
using ludus::foundation::StaticArray;

namespace
{
template <typename ValueType>
void SwapValues(ValueType& inoutLeft, ValueType& inoutRight)
{
    const ValueType left = inoutLeft;
    inoutLeft = inoutRight;
    inoutRight = left;
}
} // namespace

using ludus::foundation::int64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

TEST_CASE("Magic, version and serialized widths are frozen", "[weather_format]")
{
    STATIC_REQUIRE(format::MAGIC.size() == 8);
    STATIC_REQUIRE(format::MAGIC[7] == '\0');
    STATIC_REQUIRE(format::FORMAT_VERSION == 1);
    STATIC_REQUIRE(format::WEATHER_CHANNEL_COUNT == 14);
    STATIC_REQUIRE(format::CLIMATE_HEADER_RECORD_BYTES == 80);
    STATIC_REQUIRE(format::WEATHER_PRESET_RECORD_BYTES == 64);
    STATIC_REQUIRE(format::WEATHER_SCHEDULE_RECORD_BYTES == 16);
    STATIC_REQUIRE(format::WEATHER_SCHEDULE_ENTRY_RECORD_BYTES == 12);
    STATIC_REQUIRE(format::WEATHER_REGION_RECORD_BYTES == 48);
    STATIC_REQUIRE(format::MAX_CLIMATE_BYTES == 80 + 32 * 64 + 16 * 16 + 64 * 12 + 8 * 48);
}

TEST_CASE("Quantization rounds to the nearest code with ties toward +infinity", "[weather_format]")
{
    constexpr ludus::foundation::float64 MILLI = 1000.0;
    int64 code = 99;
    CHECK(format::TryQuantizeWeatherCode(0.0005, MILLI, -10, 10, code) == true);
    CHECK(code == 1);
    CHECK(format::TryQuantizeWeatherCode(-0.0005, MILLI, -10, 10, code) == true);
    CHECK(code == 0);
    CHECK(format::TryQuantizeWeatherCode(-0.0015, MILLI, -10, 10, code) == true);
    CHECK(code == -1);
    CHECK(format::TryQuantizeWeatherCode(0.0104, MILLI, -10, 10, code) == true);
    CHECK(code == 10);
    code = 99;
    CHECK(format::TryQuantizeWeatherCode(0.0105, MILLI, -10, 10, code) == false);
    CHECK(format::TryQuantizeWeatherCode(-0.0106, MILLI, -10, 10, code) == false);
    CHECK(format::TryQuantizeWeatherCode(std::numeric_limits<ludus::foundation::float64>::quiet_NaN(),
                                         MILLI,
                                         -10,
                                         10,
                                         code) == false);
    CHECK(format::TryQuantizeWeatherCode(std::numeric_limits<ludus::foundation::float64>::infinity(),
                                         MILLI,
                                         -10,
                                         10,
                                         code) == false);
    CHECK(code == 99);
    STATIC_REQUIRE([] {
        int64 value = 0;
        return format::TryQuantizeWeatherCode(2.5, 1.0, -10, 10, value) == true && value == 3;
    }());
}

TEST_CASE("Channel ranges and groups cover every channel", "[weather_format]")
{
    ludus::foundation::uint32 groups = 0;
    for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
    {
        const format::WeatherChannel channel = static_cast<format::WeatherChannel>(i);
        groups |= static_cast<ludus::foundation::uint32>(format::GetWeatherChannelGroup(channel));
        CHECK(format::IsWeatherChannelCodeValid(channel, 0) == true);
    }
    CHECK(groups == format::ALL_WEATHER_CHANNEL_GROUPS);
    CHECK(format::IsWeatherChannelCodeValid(format::WeatherChannel::RelativeHumidity, format::FRACTION_ONE) == true);
    CHECK(format::IsWeatherChannelCodeValid(format::WeatherChannel::RelativeHumidity, format::FRACTION_ONE + 1) ==
          false);
    CHECK(format::IsWeatherChannelCodeValid(format::WeatherChannel::Precipitation, -1) == false);
    CHECK(format::IsWeatherChannelCodeValid(format::WeatherChannel::GroundWindY, -format::MAX_WIND_COMPONENT_CODE) ==
          true);
    CHECK(format::IsWeatherChannelCodeValid(format::WeatherChannel::Temperature, format::MIN_TEMPERATURE_CODE - 1) ==
          false);
}

TEST_CASE("Record codecs round-trip little-endian fields and stop at the end", "[weather_format]")
{
    const format::WeatherRegionRecord region
    {
        .Id = 0x01020304,
        .Priority = -2,
        .ScheduleIndex = 3,
        .FeatherMillimetres = 250,
        .BoundsWorld = { .MinimumX = -5, .MinimumZ = -6, .MaximumX = 7, .MaximumZ = 8 },
    };
    StaticArray<uint8, format::WEATHER_REGION_RECORD_BYTES> bytes{};
    format::WeatherByteWriter writer{ .Bytes = std::span<uint8>(bytes.GetData(), bytes.GetSize()) };
    format::WriteWeatherRegion(writer, region);
    CHECK(writer.HasFailed == false);
    CHECK(writer.Offset == format::WEATHER_REGION_RECORD_BYTES);
    CHECK(bytes[0] == 0x04);
    CHECK(bytes[3] == 0x01);
    CHECK(bytes[4] == 0xfe);
    CHECK(bytes[7] == 0xff);

    format::WeatherByteReader reader{ .Bytes = std::span<const uint8>(bytes.GetData(), bytes.GetSize()) };
    const format::WeatherRegionRecord decoded = format::ReadWeatherRegion(reader);
    CHECK(reader.HasFailed == false);
    CHECK(decoded.Id == region.Id);
    CHECK(decoded.Priority == region.Priority);
    CHECK(decoded.FeatherMillimetres == region.FeatherMillimetres);
    CHECK(decoded.BoundsWorld == region.BoundsWorld);

    format::WeatherByteWriter shortWriter
    {
        .Bytes = std::span<uint8>(bytes.GetData(), format::WEATHER_REGION_RECORD_BYTES - 1),
    };
    format::WriteWeatherRegion(shortWriter, region);
    CHECK(shortWriter.HasFailed == true);
    format::WeatherByteReader shortReader{ .Bytes = std::span<const uint8>(bytes.GetData(), 3) };
    CHECK(shortReader.ReadUnsigned<ludus::foundation::uint32>() == 0);
    CHECK(shortReader.HasFailed == true);
}

TEST_CASE("Semantic validation enforces order, references and bounds", "[weather_format]")
{
    format::ClimateHeaderRecord header
    {
        .TicksPerSecond = 30,
        .ClimateId = 1,
        .ClimateRevision = 1,
        .DomainWorld = { .MinimumX = -100, .MinimumZ = -100, .MaximumX = 100, .MaximumZ = 100 },
        .PresetCount = 2,
        .ScheduleCount = 1,
        .ScheduleEntryCount = 1,
        .RegionCount = 2,
    };
    for (usize i = 0; i < format::MAGIC.size(); ++i)
    {
        header.Magic[i] = static_cast<uint8>(format::MAGIC[i]);
    }
    StaticArray<format::WeatherPresetRecord, 2> presets{
        format::WeatherPresetRecord{ .Id = 1, .Revision = 1 },
        format::WeatherPresetRecord{ .Id = 2, .Revision = 1 },
    };
    const StaticArray<format::WeatherScheduleRecord, 1> schedules{
        format::WeatherScheduleRecord{ .Id = 1, .FirstEntry = 0, .EntryCount = 1 },
    };
    StaticArray<format::WeatherScheduleEntryRecord, 1> entries{
        format::WeatherScheduleEntryRecord{ .PresetIndex = 1, .HoldTicks = 1 },
    };
    StaticArray<format::WeatherRegionRecord, 2> regions{
        format::WeatherRegionRecord{ .Id = 4, .Priority = 2, .BoundsWorld = {-10, -10, 10, 10} },
        format::WeatherRegionRecord{ .Id = 3, .Priority = 1, .BoundsWorld = {-10, -10, 10, 10} },
    };
    const auto validate = [&]() {
        return format::ValidateClimateRecords(
        {
            .HeaderOrNull = &header,
            .Presets = std::span<const format::WeatherPresetRecord>(presets.GetData(), presets.GetSize()),
            .Schedules = std::span<const format::WeatherScheduleRecord>(schedules.GetData(), schedules.GetSize()),
            .Entries = std::span<const format::WeatherScheduleEntryRecord>(entries.GetData(), entries.GetSize()),
            .Regions = std::span<const format::WeatherRegionRecord>(regions.GetData(), regions.GetSize()),
        });
    };
    CHECK(validate() == format::WeatherFormatStatus::Success);

    SwapValues(regions[0], regions[1]);
    CHECK(validate() == format::WeatherFormatStatus::NonCanonicalOrder);
    SwapValues(regions[0], regions[1]);
    regions[1].Id = 4;
    CHECK(validate() == format::WeatherFormatStatus::DuplicateId);
    regions[1].Id = 3;
    regions[1].BoundsWorld.MaximumX = 101;
    CHECK(validate() == format::WeatherFormatStatus::InvalidBounds);
    regions[1].BoundsWorld.MaximumX = 10;
    entries[0].PresetIndex = 2;
    CHECK(validate() == format::WeatherFormatStatus::MissingReference);
    entries[0].PresetIndex = 1;
    presets[1].Id = 1;
    CHECK(validate() == format::WeatherFormatStatus::DuplicateId);
    presets[1].Id = 2;
    presets[1].Channels.Set(format::WeatherChannel::CloudCoverage, -1);
    CHECK(validate() == format::WeatherFormatStatus::InvalidValue);
    presets[1].Channels.Set(format::WeatherChannel::CloudCoverage, 0);
    header.Reserved = 1;
    CHECK(validate() == format::WeatherFormatStatus::InvalidValue);
    header.Reserved = 0;
    CHECK(format::ValidateClimateRecords({}) == format::WeatherFormatStatus::InvalidValue);
}
