// Saved weather state: exact restore, continuation, incompatible climates and malformed saves.
#include "weather_golden_corpus.h"

#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace format = ludus::content::weather_format;
namespace weather_testing = ludus::gameplay::weather::testing;
using namespace ludus::gameplay::weather;
using ludus::foundation::Array;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;

constexpr uint32 STORM = 4;
constexpr uint32 RAIN = 3;

Climate LoadGolden(uint32 presetRevision = 1)
{
    Array<uint8> bytes;
    REQUIRE(weather_testing::TryCookGoldenClimate(false, presetRevision, bytes) == true);
    Climate climate;
    REQUIRE(TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) == format::WeatherFormatStatus::Success);
    return climate;
}

WeatherSimulation Mount(const Climate& climate)
{
    WeatherSimulation simulation;
    REQUIRE(WeatherSimulation::TryMount(climate, weather_testing::GOLDEN_TICKS_PER_SECOND, 42, simulation) ==
            WeatherSimulationStatus::Success);
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

/// Mid-blend state with an interrupted local override and a pending release.
WeatherSimulation MakeBusySimulation(const Climate& climate)
{
    WeatherSimulation simulation = Mount(climate);
    WeatherOverrideCommand storm
    {
        .OverrideId = 5,
        .ApplyTick = 3,
        .Priority = 2,
        .PresetId = STORM,
        .IsGlobal = false,
        .FootprintWorld = { .MinimumX = -500000, .MinimumZ = -500000, .MaximumX = 500000, .MaximumZ = 500000 },
        .FootprintFeatherMillimetres = 100000,
        .BlendInTicks = 10,
        .HoldTicks = 0,
        .BlendOutTicks = 6,
    };
    REQUIRE(simulation.TrySubmit(storm) == WeatherSimulationStatus::Success);
    storm.ApplyTick = 8;
    storm.PresetId = RAIN;
    REQUIRE(simulation.TrySubmit(storm) == WeatherSimulationStatus::Success);
    REQUIRE(simulation.TrySubmit(
                { .Action = WeatherOverrideAction::Release, .OverrideId = 5, .ApplyTick = 30, .BlendOutTicks = 4 }) ==
            WeatherSimulationStatus::Success);
    REQUIRE(simulation.TrySubmit(
    {
        .OverrideId = 6,
        .ApplyTick = 12,
        .PresetId = RAIN,
        .BlendInTicks = 2,
        .HoldTicks = 5,
    }) == WeatherSimulationStatus::Success);
    (void)Advance(simulation, 10);
    return simulation;
}

Array<uint8> Save(const WeatherSimulation& simulation)
{
    Array<uint8> bytes;
    REQUIRE(simulation.TrySaveState(bytes) == WeatherSimulationStatus::Success);
    return bytes;
}

bool AreStatesEqual(const WeatherSimulation& left, const WeatherSimulation& right)
{
    const Array<uint8> leftBytes = Save(left);
    const Array<uint8> rightBytes = Save(right);
    if (leftBytes.GetSize() != rightBytes.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < leftBytes.GetSize(); ++i)
    {
        if (leftBytes[i] != rightBytes[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("Saved state restores the same next tick, ordinal and continuation", "[weather][state]")
{
    const Climate climate = LoadGolden();
    WeatherSimulation original = MakeBusySimulation(climate);
    REQUIRE(original.GetState().ActiveOverrides.GetSize() == 1);
    REQUIRE(original.GetState().PendingCommands.GetSize() == 2);
    const Array<uint8> saved = Save(original);

    WeatherSimulation restored = Mount(climate);
    REQUIRE(restored.TryLoadState({saved.GetData(), saved.GetSize()}) == WeatherSimulationStatus::Success);
    CHECK(restored.GetTick() == original.GetTick());
    CHECK(restored.GetSeed() == original.GetSeed());
    CHECK(restored.GetState().NextEventOrdinal == original.GetState().NextEventOrdinal);
    CHECK(AreStatesEqual(original, restored) == true);

    for (uint32 step = 0; step < 40; ++step)
    {
        const Array<WeatherEvent> expected = Advance(original, 1);
        const Array<WeatherEvent> actual = Advance(restored, 1);
        REQUIRE(expected.GetSize() == actual.GetSize());
        for (usize i = 0; i < expected.GetSize(); ++i)
        {
            CHECK(expected[i] == actual[i]);
        }
        WeatherSample expectedSample;
        WeatherSample actualSample;
        REQUIRE(original.TrySample(-120.5, 64.0, expectedSample) == WeatherSimulationStatus::Success);
        REQUIRE(restored.TrySample(-120.5, 64.0, actualSample) == WeatherSimulationStatus::Success);
        CHECK(expectedSample.Channels == actualSample.Channels);
    }
}

TEST_CASE("A save for another preset revision is rejected with a reset repair", "[weather][state]")
{
    const WeatherSimulation original = MakeBusySimulation(LoadGolden(1));
    const Array<uint8> saved = Save(original);

    WeatherSimulation edited = Mount(LoadGolden(2));
    (void)Advance(edited, 3);
    const Array<uint8> before = Save(edited);
    CHECK(edited.TryLoadState({saved.GetData(), saved.GetSize()}) == WeatherSimulationStatus::IncompatibleClimate);
    CHECK(edited.GetTick() == 3);
    const Array<uint8> after = Save(edited);
    REQUIRE(before.GetSize() == after.GetSize());

    edited.ResetToAuthoredBaseline();
    CHECK(edited.GetTick() == 0);
    CHECK(edited.GetState().NextEventOrdinal == 0);
    CHECK(edited.GetState().ActiveOverrides.IsEmpty() == true);
    CHECK(edited.GetState().PendingCommands.IsEmpty() == true);
}

TEST_CASE("Malformed or unsupported saves leave the state unchanged", "[weather][state]")
{
    const Climate climate = LoadGolden();
    const WeatherSimulation original = MakeBusySimulation(climate);
    const Array<uint8> saved = Save(original);
    WeatherSimulation target = Mount(climate);

    // Every truncation fails.
    for (usize size = 0; size < saved.GetSize(); ++size)
    {
        CHECK(target.TryLoadState({saved.GetData(), size}) != WeatherSimulationStatus::Success);
    }
    Array<uint8> trailing = saved;
    trailing.Add(0);
    CHECK(target.TryLoadState({trailing.GetData(), trailing.GetSize()}) == WeatherSimulationStatus::InvalidState);

    constexpr usize VERSION_OFFSET = 8;
    Array<uint8> future = saved;
    future[VERSION_OFFSET] = 2;
    CHECK(target.TryLoadState({future.GetData(), future.GetSize()}) ==
          WeatherSimulationStatus::UnsupportedStateVersion);

    Array<uint8> wrongMagic = saved;
    wrongMagic[0] = 'X';
    CHECK(target.TryLoadState({wrongMagic.GetData(), wrongMagic.GetSize()}) == WeatherSimulationStatus::InvalidState);

    // Magic, four uint32 fields and three uint64 fields precede the tick; moving it past the blend is impossible.
    constexpr usize TICK_OFFSET = 48;
    Array<uint8> impossible = saved;
    impossible[TICK_OFFSET + 1] = 1;
    CHECK(target.TryLoadState({impossible.GetData(), impossible.GetSize()}) == WeatherSimulationStatus::InvalidState);

    CHECK(target.GetTick() == 0);
    CHECK(target.GetState().ActiveOverrides.IsEmpty() == true);
    CHECK(target.GetState().PendingCommands.IsEmpty() == true);

    WeatherSimulation unmounted;
    CHECK(unmounted.TryLoadState({saved.GetData(), saved.GetSize()}) == WeatherSimulationStatus::InvalidArgument);
    Array<uint8> ignored;
    CHECK(unmounted.TrySaveState(ignored) == WeatherSimulationStatus::InvalidArgument);
}
