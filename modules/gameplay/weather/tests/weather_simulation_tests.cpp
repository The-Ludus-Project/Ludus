// Hand-checked resolution, event and command rules of Ludus::GameplayWeather.
#include <ludus/content/weather/climate_cook.h>
#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <limits>

namespace
{
namespace cook = ludus::content::weather;
namespace format = ludus::content::weather_format;
using namespace ludus::gameplay::weather;
using ludus::foundation::Array;
using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

constexpr uint32 TICKS_PER_SECOND = 10;
constexpr int32 ONE = format::FRACTION_ONE;

cook::WeatherPresetSource MakeTemperaturePreset(uint32 id, float64 celsius)
{
    return { .Id = id, .TemperatureCelsius = celsius, .RelativeHumidity = 0.5, .CloudDensityScale = 1.0 };
}

struct ClimateBuilder
{
    Array<cook::WeatherPresetSource> Presets;
    Array<Array<cook::WeatherScheduleEntrySource>> Entries;
    Array<cook::WeatherScheduleSource> Schedules;
    Array<cook::WeatherRegionSource> Regions;
    uint32 DefaultScheduleId = 1;

    void AddSchedule(uint32 id, std::initializer_list<cook::WeatherScheduleEntrySource> entries, uint32 loopStart = 0)
    {
        Array<cook::WeatherScheduleEntrySource> copied;
        for (const cook::WeatherScheduleEntrySource& entry : entries)
        {
            copied.Add(entry);
        }
        Entries.Add(ludus::foundation::Move(copied));
        Schedules.Add({ .Id = id, .Entries = {}, .LoopStartEntry = loopStart });
    }

    Climate Build()
    {
        for (usize i = 0; i < Schedules.GetSize(); ++i)
        {
            Schedules[i].Entries = {Entries[i].GetData(), Entries[i].GetSize()};
        }
        const cook::ClimateSource source
        {
            .Id = 1,
            .Revision = 1,
            .TicksPerSecond = TICKS_PER_SECOND,
            .DomainWorld = { .MinimumX = -1000.0, .MinimumZ = -1000.0, .MaximumX = 1000.0, .MaximumZ = 1000.0 },
            .DefaultScheduleId = DefaultScheduleId,
            .Presets = {Presets.GetData(), Presets.GetSize()},
            .Schedules = {Schedules.GetData(), Schedules.GetSize()},
            .Regions = {Regions.GetData(), Regions.GetSize()},
        };
        Array<uint8> bytes;
        const cook::ClimateCookResult result = cook::TryCookClimate(source, bytes);
        REQUIRE(result.Status == cook::ClimateCookStatus::Success);
        Climate climate;
        REQUIRE(TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) == format::WeatherFormatStatus::Success);
        return climate;
    }
};

WeatherSimulation Mount(const Climate& climate)
{
    WeatherSimulation simulation;
    REQUIRE(WeatherSimulation::TryMount(climate, TICKS_PER_SECOND, 0, simulation) == WeatherSimulationStatus::Success);
    return simulation;
}

Array<WeatherEvent> Advance(WeatherSimulation& inoutSimulation, uint32 ticks)
{
    Array<WeatherEvent> events;
    events.Resize(usize{ticks} * MAX_WEATHER_EVENTS_PER_TICK);
    usize count = 0;
    REQUIRE(inoutSimulation.TryAdvance(ticks, {events.GetData(), events.GetSize()}, count) ==
            WeatherSimulationStatus::Success);
    events.Resize(count);
    return events;
}

WeatherSample Sample(const WeatherSimulation& simulation, float64 x, float64 z)
{
    WeatherSample sample;
    REQUIRE(simulation.TrySample(x, z, sample) == WeatherSimulationStatus::Success);
    return sample;
}

int32 Temperature(const WeatherSimulation& simulation, float64 x = 0.0, float64 z = 0.0)
{
    return Sample(simulation, x, z).Channels.Get(format::WeatherChannel::Temperature);
}

int32 SumWeights(const WeatherSample& sample)
{
    int32 sum = 0;
    for (const WeatherRegionWeight& weight : sample.RegionWeights)
    {
        sum += weight.Weight;
    }
    return sum;
}

WeatherOverrideCommand Begin(uint32 id, uint64 tick, uint32 presetId, uint32 blendIn, uint32 hold, uint32 blendOut)
{
    return
    {
        .OverrideId = id,
        .ApplyTick = tick,
        .PresetId = presetId,
        .BlendInTicks = blendIn,
        .HoldTicks = hold,
        .BlendOutTicks = blendOut,
    };
}

WeatherOverrideCommand Release(uint32 id, uint64 tick, uint32 blendOut)
{
    return { .Action = WeatherOverrideAction::Release, .OverrideId = id, .ApplyTick = tick, .BlendOutTicks = blendOut };
}
} // namespace

TEST_CASE("A held preset resolves to its exact codes everywhere", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(4, 12.345));
    builder.AddSchedule(1, {{ .PresetId = 4, .HoldTicks = 10 }});
    WeatherSimulation simulation = Mount(builder.Build());

    const WeatherSample sample = Sample(simulation, -1000.0, 999.5);
    CHECK(sample.Tick == 0);
    CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 12345);
    CHECK(sample.Channels.Get(format::WeatherChannel::RelativeHumidity) == ONE / 2);
    REQUIRE(sample.RegionWeights.GetSize() == 1);
    CHECK(sample.RegionWeights[0].RegionId == DEFAULT_WEATHER_REGION_ID);
    CHECK(sample.RegionWeights[0].Weight == ONE);
    CHECK(sample.Overrides.IsEmpty() == true);
}

TEST_CASE("Schedules hold, blend linearly, play once and loop from their loop entry", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 10.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 20.0));
    builder.Presets.Add(MakeTemperaturePreset(3, 0.0));
    // A plays once (hold 4, blend 4 into B); B (hold 2, blend 2) and C (blend 3 only) then loop.
    builder.AddSchedule(1,
                        {{ .PresetId = 1, .HoldTicks = 4, .BlendTicks = 4 },
                         { .PresetId = 2, .HoldTicks = 2, .BlendTicks = 2 },
                         { .PresetId = 3, .HoldTicks = 0, .BlendTicks = 4 }},
                        1);
    WeatherSimulation simulation = Mount(builder.Build());

    CHECK(Temperature(simulation) == 10000);
    Array<WeatherEvent> events = Advance(simulation, 4);
    CHECK(Temperature(simulation) == 10000);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0] == WeatherEvent
    {
        .Tick = 4,
        .Ordinal = 0,
        .Kind = WeatherEventKind::ScheduleBlendBegan,
        .SourceId = 0,
        .Detail = 0,
    });
    (void)Advance(simulation, 1);
    CHECK(Temperature(simulation) == 12500);
    (void)Advance(simulation, 2);
    CHECK(Temperature(simulation) == 17500);
    events = Advance(simulation, 1);
    CHECK(Temperature(simulation) == 20000);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Kind == WeatherEventKind::ScheduleEntryBegan);
    CHECK(events[0].Detail == 1);
    CHECK(events[0].Ordinal == 1);
    // Tick 11 is halfway through B's blend toward C.
    (void)Advance(simulation, 3);
    CHECK(Temperature(simulation) == 10000);
    // C at tick 12 blends straight back to B (the loop entry); a blend-only entry reports only its start.
    events = Advance(simulation, 1);
    CHECK(Temperature(simulation) == 0);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Kind == WeatherEventKind::ScheduleEntryBegan);
    CHECK(events[0].Detail == 2);
    (void)Advance(simulation, 3);
    CHECK(Temperature(simulation) == 15000);
    events = Advance(simulation, 1);
    CHECK(Temperature(simulation) == 20000);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Tick == 16);
    CHECK(events[0].Detail == 1);
}

TEST_CASE("Feathered coverage rises with a smoothstep and regional weights sum to one", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 10.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1 }});
    builder.AddSchedule(2, {{ .PresetId = 2, .HoldTicks = 1 }});
    builder.Regions.Add(
    {
        .Id = 6,
        .ScheduleId = 2,
        .BoundsWorld = { .MinimumX = 0.0, .MinimumZ = 0.0, .MaximumX = 100.0, .MaximumZ = 100.0 },
        .FeatherMetres = 20.0,
    });
    const WeatherSimulation simulation = Mount(builder.Build());

    // Depth 10 m of 20 m: smoothstep(0.5) = 0.5.
    WeatherSample sample = Sample(simulation, 10.0, 50.0);
    CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 5000);
    REQUIRE(sample.RegionWeights.GetSize() == 2);
    CHECK(sample.RegionWeights[0].RegionId == 6);
    CHECK(sample.RegionWeights[0].Weight == ONE / 2);
    CHECK(sample.RegionWeights[1].RegionId == DEFAULT_WEATHER_REGION_ID);
    CHECK(SumWeights(sample) == ONE);
    // Depth 5 m: smoothstep(0.25) = 0.15625 -> weight 10240, temperature 1562.5 rounds away from zero.
    sample = Sample(simulation, 50.0, 95.0);
    CHECK(sample.RegionWeights[0].Weight == 10240);
    CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 1563);
    CHECK(SumWeights(sample) == ONE);
    // The edge itself and outside belong to the default region; beyond the feather the region is whole.
    CHECK(Temperature(simulation, 0.0, 50.0) == 0);
    CHECK(Temperature(simulation, -0.25, 50.0) == 0);
    CHECK(Temperature(simulation, 20.0, 50.0) == 10000);
    sample = Sample(simulation, 50.0, 50.0);
    REQUIRE(sample.RegionWeights.GetSize() == 1);
    CHECK(sample.RegionWeights[0].Weight == ONE);
}

TEST_CASE("Higher-priority regions claim coverage first and equal priorities resolve by ID", "[weather]")
{
    const cook::WeatherRegionSource low
    {
        .Id = 2,
        .Priority = 1,
        .ScheduleId = 2,
        .BoundsWorld = { .MinimumX = -50.0, .MinimumZ = -50.0, .MaximumX = 50.0, .MaximumZ = 50.0 },
    };
    cook::WeatherRegionSource high = low;
    high.Id = 9;
    high.ScheduleId = 3;
    for (const int32 highPriority : {5, 1})
    {
        ClimateBuilder builder;
        builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
        builder.Presets.Add(MakeTemperaturePreset(2, 10.0));
        builder.Presets.Add(MakeTemperaturePreset(3, 30.0));
        builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1 }});
        builder.AddSchedule(2, {{ .PresetId = 2, .HoldTicks = 1 }});
        builder.AddSchedule(3, {{ .PresetId = 3, .HoldTicks = 1 }});
        high.Priority = highPriority;
        builder.Regions.Add(high);
        builder.Regions.Add(low);
        const WeatherSimulation simulation = Mount(builder.Build());
        const WeatherSample sample = Sample(simulation, 0.0, 0.0);
        REQUIRE(sample.RegionWeights.GetSize() == 1);
        if (highPriority == 5)
        {
            CHECK(sample.RegionWeights[0].RegionId == 9);
            CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 30000);
        }
        else
        {
            CHECK(sample.RegionWeights[0].RegionId == 2);
            CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 10000);
        }
    }
}

TEST_CASE("An override blends in, holds, expires and returns to the running schedule", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 40.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1000 }});
    WeatherSimulation simulation = Mount(builder.Build());
    REQUIRE(simulation.TrySubmit(Begin(8, 2, 2, 4, 3, 2)) == WeatherSimulationStatus::Success);

    Array<WeatherEvent> events = Advance(simulation, 2);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0] == WeatherEvent
    {
        .Tick = 2,
        .Ordinal = 0,
        .Kind = WeatherEventKind::OverrideStarted,
        .SourceId = 8,
        .Detail = 2,
    });
    CHECK(Temperature(simulation) == 0);
    (void)Advance(simulation, 1);
    CHECK(Temperature(simulation) == 10000);
    const WeatherSample blending = Sample(simulation, 0.0, 0.0);
    REQUIRE(blending.Overrides.GetSize() == 1);
    CHECK(blending.Overrides[0].Weight == ONE / 4);
    (void)Advance(simulation, 3);
    CHECK(Temperature(simulation) == 40000);
    CHECK(simulation.GetState().ActiveOverrides[0].Phase == WeatherOverridePhase::Hold);
    events = Advance(simulation, 3);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Tick == 9);
    CHECK(events[0].Kind == WeatherEventKind::OverrideExpired);
    CHECK(Temperature(simulation) == 40000);
    (void)Advance(simulation, 1);
    CHECK(Temperature(simulation) == 20000);
    events = Advance(simulation, 1);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Tick == 11);
    CHECK(events[0].Kind == WeatherEventKind::OverrideEnded);
    CHECK(Temperature(simulation) == 0);
    CHECK(simulation.GetState().ActiveOverrides.IsEmpty() == true);
}

TEST_CASE("An interruption continues from the current weight and values at every position", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 30.0));
    builder.Presets.Add(MakeTemperaturePreset(3, 10.0));
    builder.Presets.Add(MakeTemperaturePreset(4, 20.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1000 }});
    builder.AddSchedule(2, {{ .PresetId = 4, .HoldTicks = 1000 }});
    builder.Regions.Add(
    {
        .Id = 3,
        .ScheduleId = 2,
        .BoundsWorld = { .MinimumX = 0.0, .MinimumZ = -100.0, .MaximumX = 200.0, .MaximumZ = 100.0 },
        .FeatherMetres = 100.0,
    });
    WeatherSimulation simulation = Mount(builder.Build());
    WeatherOverrideCommand first = Begin(5, 1, 2, 10, 0, 4);
    first.IsGlobal = false;
    first.FootprintWorld = { .MinimumX = -100000, .MinimumZ = -100000, .MaximumX = 100000, .MaximumZ = 100000 };
    first.FootprintFeatherMillimetres = 50000;
    WeatherOverrideCommand second = first;
    second.ApplyTick = 6;
    second.PresetId = 3;
    REQUIRE(simulation.TrySubmit(first) == WeatherSimulationStatus::Success);
    REQUIRE(simulation.TrySubmit(second) == WeatherSimulationStatus::Success);

    const ludus::foundation::StaticArray<float64, 5> positions{-80.0, -25.0, 0.0, 40.0, 75.0};
    (void)Advance(simulation, 5);
    // Expected values at tick 6 had the first blend continued; the interruption must start from them.
    WeatherSimulation continued = Mount(simulation.GetClimate());
    REQUIRE(continued.TrySubmit(first) == WeatherSimulationStatus::Success);
    (void)Advance(continued, 6);
    const Array<WeatherEvent> events = Advance(simulation, 1);
    REQUIRE(events.GetSize() == 1);
    CHECK(events[0].Kind == WeatherEventKind::OverrideInterrupted);
    CHECK(events[0].Detail == 3);
    for (const float64 x : positions)
    {
        CHECK(Temperature(simulation, x, 0.0) == Temperature(continued, x, 0.0));
    }
    // Five ticks later at the footprint core (outside the region): weight 0.5 -> 0.75, source 30 -> 20 degrees.
    (void)Advance(simulation, 5);
    CHECK(Temperature(simulation, 0.0, 0.0) == 15000);
    (void)Advance(simulation, 5);
    CHECK(Temperature(simulation, 0.0, 0.0) == 10000);
}

TEST_CASE("Channel groups restrict what an override replaces", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 5.0));
    cook::WeatherPresetSource wet = MakeTemperaturePreset(2, 25.0);
    wet.PrecipitationMillimetresPerHour = 8.0;
    builder.Presets.Add(wet);
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 100 }});
    WeatherSimulation simulation = Mount(builder.Build());
    WeatherOverrideCommand rain = Begin(1, 1, 2, 0, 0, 0);
    rain.ChannelGroups = static_cast<uint32>(format::WeatherChannelGroup::Precipitation);
    REQUIRE(simulation.TrySubmit(rain) == WeatherSimulationStatus::Success);
    (void)Advance(simulation, 1);
    const WeatherSample sample = Sample(simulation, 0.0, 0.0);
    CHECK(sample.Channels.Get(format::WeatherChannel::Precipitation) == 8000);
    CHECK(sample.Channels.Get(format::WeatherChannel::Temperature) == 5000);
}

TEST_CASE("Commands that cannot apply at their tick produce rejection events", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 10.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1000 }});
    WeatherSimulation simulation = Mount(builder.Build());
    REQUIRE(simulation.TrySubmit(Release(3, 1, 0)) == WeatherSimulationStatus::Success);
    for (uint32 id = 1; id <= MAX_ACTIVE_WEATHER_OVERRIDES + 1; ++id)
    {
        REQUIRE(simulation.TrySubmit(Begin(id + 10, 2, 2, 1, 0, 1)) == WeatherSimulationStatus::Success);
    }
    WeatherOverrideCommand moved = Begin(11, 3, 2, 1, 0, 1);
    moved.Priority = 4;
    REQUIRE(simulation.TrySubmit(moved) == WeatherSimulationStatus::Success);

    const Array<WeatherEvent> events = Advance(simulation, 3);
    REQUIRE(events.GetSize() == 1 + MAX_ACTIVE_WEATHER_OVERRIDES + 1 + 1);
    CHECK(events[0].Kind == WeatherEventKind::OverrideRejected);
    CHECK(events[0].Detail == static_cast<uint32>(WeatherOverrideRejection::NotActive));
    const WeatherEvent& full = events[MAX_ACTIVE_WEATHER_OVERRIDES + 1];
    CHECK(full.Kind == WeatherEventKind::OverrideRejected);
    CHECK(full.SourceId == MAX_ACTIVE_WEATHER_OVERRIDES + 11);
    CHECK(full.Detail == static_cast<uint32>(WeatherOverrideRejection::CapacityExceeded));
    const WeatherEvent& mismatch = events[MAX_ACTIVE_WEATHER_OVERRIDES + 2];
    CHECK(mismatch.Tick == 3);
    CHECK(mismatch.Detail == static_cast<uint32>(WeatherOverrideRejection::FootprintMismatch));
    for (usize i = 0; i < events.GetSize(); ++i)
    {
        CHECK(events[i].Ordinal == i);
    }
}

TEST_CASE("Submission validates commands against the climate and the clock", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 10 }});
    WeatherSimulation simulation = Mount(builder.Build());
    (void)Advance(simulation, 2);

    CHECK(simulation.TrySubmit(Begin(1, 2, 1, 0, 0, 0)) == WeatherSimulationStatus::CommandInPast);
    CHECK(simulation.TrySubmit(Begin(0, 3, 1, 0, 0, 0)) == WeatherSimulationStatus::InvalidArgument);
    CHECK(simulation.TrySubmit(Begin(1, 3, 99, 0, 0, 0)) == WeatherSimulationStatus::UnknownPreset);
    WeatherOverrideCommand command = Begin(1, 3, 1, 0, 0, 0);
    command.ChannelGroups = 0;
    CHECK(simulation.TrySubmit(command) == WeatherSimulationStatus::InvalidArgument);
    command.ChannelGroups = format::ALL_WEATHER_CHANNEL_GROUPS + 1;
    CHECK(simulation.TrySubmit(command) == WeatherSimulationStatus::InvalidArgument);
    command.ChannelGroups = format::ALL_WEATHER_CHANNEL_GROUPS;
    command.IsGlobal = false;
    command.FootprintWorld = { .MinimumX = 0, .MinimumZ = 0, .MaximumX = 2000000, .MaximumZ = 10 };
    CHECK(simulation.TrySubmit(command) == WeatherSimulationStatus::InvalidFootprint);
    command.FootprintWorld.MaximumX = 0;
    CHECK(simulation.TrySubmit(command) == WeatherSimulationStatus::InvalidFootprint);
    CHECK(simulation.GetState().PendingCommands.IsEmpty() == true);

    REQUIRE(simulation.TrySubmit(Begin(1, 3, 1, 0, 0, 0)) == WeatherSimulationStatus::Success);
    CHECK(simulation.TrySubmit(Begin(1, 3, 1, 5, 0, 0)) == WeatherSimulationStatus::DuplicateCommand);
    CHECK(simulation.TrySubmit(Release(1, 3, 0)) == WeatherSimulationStatus::Success);
    for (uint32 i = 2; i < MAX_PENDING_WEATHER_COMMANDS; ++i)
    {
        REQUIRE(simulation.TrySubmit(Begin(i, 4, 1, 0, 0, 0)) == WeatherSimulationStatus::Success);
    }
    CHECK(simulation.TrySubmit(Begin(99, 4, 1, 0, 0, 0)) == WeatherSimulationStatus::CapacityExceeded);
}

TEST_CASE("Submission order within a tick does not change the outcome", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.Presets.Add(MakeTemperaturePreset(2, 10.0));
    builder.Presets.Add(MakeTemperaturePreset(3, 30.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 100 }});
    const Climate climate = builder.Build();
    WeatherSimulation forward = Mount(climate);
    WeatherSimulation reverse = Mount(climate);
    WeatherOverrideCommand a = Begin(4, 1, 2, 2, 3, 2);
    WeatherOverrideCommand b = Begin(2, 1, 3, 1, 0, 1);
    b.Priority = 3;
    const WeatherOverrideCommand c = Release(4, 3, 1);
    for (const WeatherOverrideCommand& command : {a, b, c})
    {
        REQUIRE(forward.TrySubmit(command) == WeatherSimulationStatus::Success);
    }
    for (const WeatherOverrideCommand& command : {c, b, a})
    {
        REQUIRE(reverse.TrySubmit(command) == WeatherSimulationStatus::Success);
    }
    for (uint32 tick = 0; tick < 8; ++tick)
    {
        const Array<WeatherEvent> forwardEvents = Advance(forward, 1);
        const Array<WeatherEvent> reverseEvents = Advance(reverse, 1);
        REQUIRE(forwardEvents.GetSize() == reverseEvents.GetSize());
        for (usize i = 0; i < forwardEvents.GetSize(); ++i)
        {
            CHECK(forwardEvents[i] == reverseEvents[i]);
        }
        CHECK(Sample(forward, 0.0, 0.0).Channels == Sample(reverse, 0.0, 0.0).Channels);
    }
}

TEST_CASE("Advancing is transactional and rejects invalid queries", "[weather]")
{
    ClimateBuilder builder;
    builder.Presets.Add(MakeTemperaturePreset(1, 0.0));
    builder.AddSchedule(1, {{ .PresetId = 1, .HoldTicks = 1 }});
    WeatherSimulation simulation = Mount(builder.Build());

    // Every tick starts a one-tick entry, so three ticks need three events.
    ludus::foundation::StaticArray<WeatherEvent, 2> storage{};
    const ludus::foundation::BorrowedView<WeatherEvent> events(storage.GetData(), storage.GetSize());
    usize count = 7;
    CHECK(simulation.TryAdvance(3, events, count) == WeatherSimulationStatus::CapacityExceeded);
    CHECK(simulation.GetTick() == 0);
    CHECK(simulation.GetState().NextEventOrdinal == 0);
    CHECK(count == 7);
    CHECK(simulation.TryAdvance(2, events, count) == WeatherSimulationStatus::Success);
    CHECK(count == 2);
    CHECK(simulation.TryAdvance(0, events, count) == WeatherSimulationStatus::Success);
    CHECK(count == 0);

    WeatherSample sample;
    sample.Tick = 99;
    CHECK(simulation.TrySample(1000.5, 0.0, sample) == WeatherSimulationStatus::OutsideDomain);
    CHECK(simulation.TrySample(0.0, -1000.5, sample) == WeatherSimulationStatus::OutsideDomain);
    CHECK(simulation.TrySample(std::numeric_limits<float64>::quiet_NaN(), 0.0, sample) ==
          WeatherSimulationStatus::OutsideDomain);
    CHECK(simulation.TrySample(0.0, std::numeric_limits<float64>::infinity(), sample) ==
          WeatherSimulationStatus::OutsideDomain);
    CHECK(sample.Tick == 99);
    CHECK(simulation.TrySample(1000.0, -1000.0, sample) == WeatherSimulationStatus::Success);

    WeatherSimulation wrongRate;
    CHECK(WeatherSimulation::TryMount(simulation.GetClimate(), TICKS_PER_SECOND + 1, 0, wrongRate) ==
          WeatherSimulationStatus::TickRateMismatch);
    CHECK(wrongRate.IsMounted() == false);
    CHECK(WeatherSimulation::TryMount(Climate{}, TICKS_PER_SECOND, 0, wrongRate) ==
          WeatherSimulationStatus::InvalidArgument);
    CHECK(wrongRate.TrySample(0.0, 0.0, sample) == WeatherSimulationStatus::InvalidArgument);
}

TEST_CASE("Presentation conversion reports natural units", "[weather]")
{
    format::WeatherChannelCodes codes;
    codes.Set(format::WeatherChannel::Temperature, -2500);
    codes.Set(format::WeatherChannel::RelativeHumidity, ONE / 4);
    codes.Set(format::WeatherChannel::CloudWindX, -12500);
    codes.Set(format::WeatherChannel::FogExtinction, 2000);
    codes.Set(format::WeatherChannel::Precipitation, 7500);
    const WeatherConditions conditions = ConvertWeatherChannels(codes);
    CHECK(conditions.TemperatureCelsius == -2.5);
    CHECK(conditions.RelativeHumidity == 0.25);
    CHECK(conditions.CloudWindWorld.X == -12.5);
    CHECK(conditions.FogExtinctionPerMetre == 0.002);
    CHECK(conditions.PrecipitationMillimetresPerHour == 7.5);
}
