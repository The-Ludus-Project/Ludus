// Climate cooker: canonical bytes, quantization, diagnostics, and the loader's matching rejections.
#include <ludus/content/weather/climate_cook.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/weather/climate.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
template <typename ValueType>
void SwapValues(ValueType& inoutLeft, ValueType& inoutRight)
{
    const ValueType left = inoutLeft;
    inoutLeft = inoutRight;
    inoutRight = left;
}

namespace cook = ludus::content::weather;
namespace format = ludus::content::weather_format;
namespace runtime = ludus::gameplay::weather;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;

struct Fixture
{
    StaticArray<cook::WeatherPresetSource, 2> Presets{
        cook::WeatherPresetSource{ .Id = 9, .TemperatureCelsius = -3.0005, .RelativeHumidity = 0.25 },
        cook::WeatherPresetSource
        {
            .Id = 2,
            .Revision = 3,
            .TemperatureCelsius = 18.0,
            .GroundWindWorld = {1.25, 0.0, -2.5},
            .CloudCoverage = 0.75,
            .PrecipitationMillimetresPerHour = 4.5,
        },
    };
    StaticArray<cook::WeatherScheduleEntrySource, 2> DefaultEntries{
        cook::WeatherScheduleEntrySource{ .PresetId = 2, .HoldTicks = 10, .BlendTicks = 5 },
        cook::WeatherScheduleEntrySource{ .PresetId = 9, .HoldTicks = 7 },
    };
    StaticArray<cook::WeatherScheduleEntrySource, 1> RegionEntries{
        cook::WeatherScheduleEntrySource{ .PresetId = 9, .HoldTicks = 1 },
    };
    StaticArray<cook::WeatherScheduleSource, 2> Schedules{
        cook::WeatherScheduleSource{ .Id = 40, .Entries = {RegionEntries.GetData(), 1} },
        cook::WeatherScheduleSource{ .Id = 4, .Entries = {DefaultEntries.GetData(), 2}, .LoopStartEntry = 1 },
    };
    StaticArray<cook::WeatherRegionSource, 3> Regions{
        cook::WeatherRegionSource
        {
            .Id = 8,
            .Priority = 1,
            .ScheduleId = 40,
            .BoundsWorld = { .MinimumX = -10.0, .MinimumZ = -10.0, .MaximumX = 10.0, .MaximumZ = 10.0 },
            .FeatherMetres = 2.5,
        },
        cook::WeatherRegionSource
        {
            .Id = 3,
            .Priority = 1,
            .ScheduleId = 4,
            .BoundsWorld = { .MinimumX = 0.0, .MinimumZ = 0.0, .MaximumX = 5.0, .MaximumZ = 5.0 },
        },
        cook::WeatherRegionSource
        {
            .Id = 5,
            .Priority = 7,
            .ScheduleId = 40,
            .BoundsWorld = { .MinimumX = -1.0, .MinimumZ = -1.0, .MaximumX = 1.0, .MaximumZ = 1.0 },
        },
    };

    cook::ClimateSource MakeSource()
    {
        Schedules[0].Entries = {RegionEntries.GetData(), RegionEntries.GetSize()};
        Schedules[1].Entries = {DefaultEntries.GetData(), DefaultEntries.GetSize()};
        return
        {
            .Id = 11,
            .Revision = 2,
            .TicksPerSecond = 60,
            .DomainWorld = { .MinimumX = -100.0, .MinimumZ = -100.0, .MaximumX = 100.0, .MaximumZ = 100.0 },
            .DefaultScheduleId = 4,
            .Presets = {Presets.GetData(), Presets.GetSize()},
            .Schedules = {Schedules.GetData(), Schedules.GetSize()},
            .Regions = {Regions.GetData(), Regions.GetSize()},
        };
    }
};

Array<uint8> CookOrFail(const cook::ClimateSource& source)
{
    Array<uint8> bytes;
    const cook::ClimateCookResult result = cook::TryCookClimate(source, bytes);
    REQUIRE(result.Status == cook::ClimateCookStatus::Success);
    return bytes;
}

bool AreEqual(const Array<uint8>& left, const Array<uint8>& right)
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

cook::ClimateCookResult CookExpectingFailure(const cook::ClimateSource& source)
{
    Array<uint8> bytes;
    bytes.Add(0xab);
    const cook::ClimateCookResult result = cook::TryCookClimate(source, bytes);
    CHECK(bytes.GetSize() == 1);
    CHECK(bytes[0] == 0xab);
    return result;
}
} // namespace

TEST_CASE("Cooking writes canonical, quantized records that the runtime loads", "[weather][cook]")
{
    Fixture fixture;
    const Array<uint8> bytes = CookOrFail(fixture.MakeSource());
    CHECK(bytes.GetSize() == format::CLIMATE_HEADER_RECORD_BYTES + 2 * format::WEATHER_PRESET_RECORD_BYTES +
                                 2 * format::WEATHER_SCHEDULE_RECORD_BYTES +
                                 3 * format::WEATHER_SCHEDULE_ENTRY_RECORD_BYTES +
                                 3 * format::WEATHER_REGION_RECORD_BYTES);

    runtime::Climate climate;
    REQUIRE(runtime::TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) ==
            format::WeatherFormatStatus::Success);
    const format::ClimateHeaderRecord& header = climate.GetHeader();
    CHECK(header.ClimateId == 11);
    CHECK(header.ClimateRevision == 2);
    CHECK(header.TicksPerSecond == 60);
    CHECK(header.DomainWorld == format::WeatherBoundsWorld{-100000, -100000, 100000, 100000});

    // Presets and schedules ascend by ID; entries follow their schedule; IDs resolve to indices.
    REQUIRE(climate.GetPresets().GetSize() == 2);
    CHECK(climate.GetPresets()[0].Id == 2);
    CHECK(climate.GetPresets()[0].Revision == 3);
    CHECK(climate.GetPresets()[0].Channels.Get(format::WeatherChannel::GroundWindX) == 1250);
    CHECK(climate.GetPresets()[0].Channels.Get(format::WeatherChannel::GroundWindZ) == -2500);
    CHECK(climate.GetPresets()[0].Channels.Get(format::WeatherChannel::CloudCoverage) == 49152);
    CHECK(climate.GetPresets()[0].Channels.Get(format::WeatherChannel::Precipitation) == 4500);
    CHECK(climate.GetPresets()[1].Id == 9);
    // -3000.5 millidegrees rounds toward +infinity.
    CHECK(climate.GetPresets()[1].Channels.Get(format::WeatherChannel::Temperature) == -3000);
    REQUIRE(climate.GetSchedules().GetSize() == 2);
    CHECK(climate.GetSchedules()[0].Id == 4);
    CHECK(climate.GetSchedules()[0].FirstEntry == 0);
    CHECK(climate.GetSchedules()[0].LoopStartEntry == 1);
    CHECK(climate.GetSchedules()[1].Id == 40);
    CHECK(climate.GetSchedules()[1].FirstEntry == 2);
    CHECK(climate.GetScheduleEntries()[0].PresetIndex == 0);
    CHECK(climate.GetScheduleEntries()[1].PresetIndex == 1);
    CHECK(header.DefaultScheduleIndex == 0);

    // Regions are in resolution order: priority 7 first, then priority 1 by ascending ID.
    REQUIRE(climate.GetRegions().GetSize() == 3);
    CHECK(climate.GetRegions()[0].Id == 5);
    CHECK(climate.GetRegions()[1].Id == 3);
    CHECK(climate.GetRegions()[1].ScheduleIndex == 0);
    CHECK(climate.GetRegions()[2].Id == 8);
    CHECK(climate.GetRegions()[2].FeatherMillimetres == 2500);

    uint32 index = 0;
    CHECK(climate.TryFindPresetIndex(9, index) == true);
    CHECK(index == 1);
    CHECK(climate.TryFindPresetIndex(3, index) == false);
}

TEST_CASE("Authoring order does not change the cooked bytes", "[weather][cook]")
{
    Fixture forward;
    Fixture reversed;
    SwapValues(reversed.Presets[0], reversed.Presets[1]);
    SwapValues(reversed.Schedules[0], reversed.Schedules[1]);
    SwapValues(reversed.Regions[0], reversed.Regions[2]);
    // Swapping the schedules swaps which entry arrays their views name, so rebuild the views by ID.
    cook::ClimateSource source = reversed.MakeSource();
    StaticArray<cook::WeatherScheduleSource, 2> schedules{
        cook::WeatherScheduleSource
        {
            .Id = 4,
            .Entries = {reversed.DefaultEntries.GetData(), reversed.DefaultEntries.GetSize()},
            .LoopStartEntry = 1,
        },
        cook::WeatherScheduleSource{ .Id = 40, .Entries = {reversed.RegionEntries.GetData(), 1} },
    };
    source.Schedules = {schedules.GetData(), schedules.GetSize()};
    CHECK(AreEqual(CookOrFail(forward.MakeSource()), CookOrFail(source)) == true);
}

TEST_CASE("Cook failures name the authored record and preserve the output", "[weather][cook]")
{
    SECTION("Duplicate preset")
    {
        Fixture fixture;
        fixture.Presets[1].Id = 9;
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::DuplicateId);
        CHECK(result.Subject == cook::ClimateCookSubject::Preset);
        CHECK(result.SubjectId == 9);
    }
    SECTION("Out-of-range channel")
    {
        Fixture fixture;
        fixture.Presets[0].RelativeHumidity = 1.5;
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::ValueOutOfRange);
        CHECK(result.SubjectId == 9);
        CHECK(result.Detail == static_cast<uint32>(format::WeatherChannel::RelativeHumidity));
    }
    SECTION("Nonfinite channel")
    {
        Fixture fixture;
        fixture.Presets[1].CloudWindWorld.Z = std::numeric_limits<ludus::foundation::float64>::quiet_NaN();
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::ValueOutOfRange);
        CHECK(result.Detail == static_cast<uint32>(format::WeatherChannel::CloudWindZ));
    }
    SECTION("Missing preset in a schedule entry")
    {
        Fixture fixture;
        fixture.DefaultEntries[1].PresetId = 77;
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::MissingReference);
        CHECK(result.Subject == cook::ClimateCookSubject::ScheduleEntry);
        CHECK(result.SubjectId == 4);
        CHECK(result.Detail == 1);
    }
    SECTION("Empty schedule entry")
    {
        Fixture fixture;
        fixture.DefaultEntries[0].HoldTicks = 0;
        fixture.DefaultEntries[0].BlendTicks = 0;
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::InvalidArgument);
        CHECK(result.Subject == cook::ClimateCookSubject::ScheduleEntry);
    }
    SECTION("Region outside the domain")
    {
        Fixture fixture;
        fixture.Regions[1].BoundsWorld.MaximumX = 100.5;
        const cook::ClimateCookResult result = CookExpectingFailure(fixture.MakeSource());
        CHECK(result.Status == cook::ClimateCookStatus::InvalidBounds);
        CHECK(result.Subject == cook::ClimateCookSubject::Region);
        CHECK(result.SubjectId == 3);
    }
    SECTION("Unknown default schedule")
    {
        Fixture fixture;
        cook::ClimateSource source = fixture.MakeSource();
        source.DefaultScheduleId = 41;
        const cook::ClimateCookResult result = CookExpectingFailure(source);
        CHECK(result.Status == cook::ClimateCookStatus::MissingReference);
        CHECK(result.Subject == cook::ClimateCookSubject::Climate);
    }
    SECTION("Tick rate out of range")
    {
        Fixture fixture;
        cook::ClimateSource source = fixture.MakeSource();
        source.TicksPerSecond = format::MAX_TICKS_PER_SECOND + 1;
        CHECK(CookExpectingFailure(source).Status == cook::ClimateCookStatus::InvalidArgument);
    }
    SECTION("Too many regions")
    {
        Fixture fixture;
        StaticArray<cook::WeatherRegionSource, format::MAX_WEATHER_REGIONS + 1> regions{};
        for (usize i = 0; i < regions.GetSize(); ++i)
        {
            regions[i] = fixture.Regions[0];
            regions[i].Id = static_cast<uint32>(i + 1);
        }
        cook::ClimateSource source = fixture.MakeSource();
        source.Regions = {regions.GetData(), regions.GetSize()};
        CHECK(CookExpectingFailure(source).Status == cook::ClimateCookStatus::CapacityExceeded);
    }
}

TEST_CASE("The loader rejects malformed blobs with the matching format rule", "[weather][cook]")
{
    Fixture fixture;
    const Array<uint8> bytes = CookOrFail(fixture.MakeSource());
    runtime::Climate climate;
    for (usize size = 0; size < bytes.GetSize(); ++size)
    {
        CHECK(runtime::TryLoadClimate({bytes.GetData(), size}, climate) == format::WeatherFormatStatus::Truncated);
    }
    Array<uint8> trailing = bytes;
    trailing.Add(0);
    CHECK(runtime::TryLoadClimate({trailing.GetData(), trailing.GetSize()}, climate) ==
          format::WeatherFormatStatus::TrailingBytes);
    Array<uint8> magic = bytes;
    magic[0] = 'X';
    CHECK(runtime::TryLoadClimate({magic.GetData(), magic.GetSize()}, climate) ==
          format::WeatherFormatStatus::InvalidMagic);
    constexpr usize VERSION_OFFSET = 8;
    Array<uint8> version = bytes;
    version[VERSION_OFFSET] = 2;
    CHECK(runtime::TryLoadClimate({version.GetData(), version.GetSize()}, climate) ==
          format::WeatherFormatStatus::UnsupportedVersion);
    // Swap the two preset IDs (first field of each preset record) to break canonical order.
    constexpr usize FIRST_PRESET = format::CLIMATE_HEADER_RECORD_BYTES;
    constexpr usize SECOND_PRESET = FIRST_PRESET + format::WEATHER_PRESET_RECORD_BYTES;
    Array<uint8> order = bytes;
    SwapValues(order[FIRST_PRESET], order[SECOND_PRESET]);
    CHECK(runtime::TryLoadClimate({order.GetData(), order.GetSize()}, climate) ==
          format::WeatherFormatStatus::NonCanonicalOrder);
    CHECK(climate.IsEmpty() == true);
    CHECK(runtime::TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) == format::WeatherFormatStatus::Success);
    CHECK(climate.IsEmpty() == false);
}
