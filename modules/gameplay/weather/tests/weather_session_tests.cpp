// Session control semantics: pause, step, preview scrub, reset, and region boundaries seen from distinct views.
#include "weather_golden_corpus.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace format = ludus::content::weather_format;
namespace weather_testing = ludus::gameplay::weather::testing;
using namespace ludus::gameplay::weather;
using ludus::foundation::Array;
using ludus::foundation::float64;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

constexpr uint32 RAIN = 3;
constexpr uint32 STORM = 4;

struct ViewPosition
{
    float64 XWorld;
    float64 ZWorld;
};

// Three cameras across the valley's western feather (X -800 m, 200 m deep) and one in the default region.
constexpr usize VIEW_COUNT = 4;
constexpr StaticArray<ViewPosition, VIEW_COUNT> VIEWS{
    ViewPosition{-820.0, 0.0},
    ViewPosition{-750.0, 0.0},
    ViewPosition{-650.0, 0.0},
    ViewPosition{-500.0, 0.0},
};

WeatherSimulation MountGolden()
{
    Array<uint8> bytes;
    REQUIRE(weather_testing::TryCookGoldenClimate(false, 1, bytes) == true);
    Climate climate;
    REQUIRE(TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) == format::WeatherFormatStatus::Success);
    WeatherSimulation simulation;
    REQUIRE(WeatherSimulation::TryMount(climate, weather_testing::GOLDEN_TICKS_PER_SECOND, 7, simulation) ==
            WeatherSimulationStatus::Success);
    return simulation;
}

void SubmitStorm(WeatherSimulation& inoutSimulation)
{
    WeatherOverrideCommand storm
    {
        .OverrideId = 5,
        .ApplyTick = 4,
        .Priority = 2,
        .PresetId = STORM,
        .IsGlobal = false,
        .FootprintWorld = { .MinimumX = -900000, .MinimumZ = -500000, .MaximumX = -100000, .MaximumZ = 500000 },
        .FootprintFeatherMillimetres = 80000,
        .BlendInTicks = 12,
        .HoldTicks = 0,
        .BlendOutTicks = 9,
    };
    REQUIRE(inoutSimulation.TrySubmit(storm) == WeatherSimulationStatus::Success);
    storm.ApplyTick = 10;
    storm.PresetId = RAIN;
    REQUIRE(inoutSimulation.TrySubmit(storm) == WeatherSimulationStatus::Success);
    REQUIRE(inoutSimulation.TrySubmit(
    {
        .Action = WeatherOverrideAction::Release,
        .OverrideId = 5,
        .ApplyTick = 40,
        .BlendOutTicks = 6,
    }) == WeatherSimulationStatus::Success);
    REQUIRE(inoutSimulation.TrySubmit(
    {
        .OverrideId = 8,
        .ApplyTick = 25,
        .Priority = 4,
        .PresetId = RAIN,
        .BlendInTicks = 3,
        .HoldTicks = 30,
        .BlendOutTicks = 5,
    }) == WeatherSimulationStatus::Success);
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

WeatherSample Sample(const WeatherSimulation& simulation, const ViewPosition& view)
{
    WeatherSample sample;
    REQUIRE(simulation.TrySample(view.XWorld, view.ZWorld, sample) == WeatherSimulationStatus::Success);
    return sample;
}

WeatherSample
Preview(const WeatherSimulation& simulation, WeatherPreviewMode mode, uint64 tick, const ViewPosition& view)
{
    WeatherSample sample;
    REQUIRE(simulation.TryPreview(mode, tick, view.XWorld, view.ZWorld, sample) == WeatherSimulationStatus::Success);
    return sample;
}

Array<uint8> Save(const WeatherSimulation& simulation)
{
    Array<uint8> bytes;
    REQUIRE(simulation.TrySaveState(bytes) == WeatherSimulationStatus::Success);
    return bytes;
}

bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right)
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

bool AreSamplesEqual(const WeatherSample& left, const WeatherSample& right)
{
    if (left.Tick != right.Tick || (left.Channels == right.Channels) == false ||
        left.RegionWeights.GetSize() != right.RegionWeights.GetSize() ||
        left.Overrides.GetSize() != right.Overrides.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.RegionWeights.GetSize(); ++i)
    {
        if (left.RegionWeights[i].RegionId != right.RegionWeights[i].RegionId ||
            left.RegionWeights[i].Weight != right.RegionWeights[i].Weight)
        {
            return false;
        }
    }
    for (usize i = 0; i < left.Overrides.GetSize(); ++i)
    {
        if (left.Overrides[i].OverrideId != right.Overrides[i].OverrideId ||
            left.Overrides[i].Weight != right.Overrides[i].Weight)
        {
            return false;
        }
    }
    return true;
}

ludus::foundation::int32 GetRegionWeight(const WeatherSample& sample, uint32 regionId)
{
    for (const WeatherRegionWeight& weight : sample.RegionWeights)
    {
        if (weight.RegionId == regionId)
        {
            return weight.Weight;
        }
    }
    return 0;
}
} // namespace

TEST_CASE("While paused, sampling any view changes nothing and a step advances exactly one tick", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    SubmitStorm(simulation);
    (void)Advance(simulation, 12);
    const Array<uint8> pausedState = Save(simulation);

    // Paused frames still render: every view samples the same tick as often and in any order.
    StaticArray<WeatherSample, VIEW_COUNT> first{};
    for (usize i = 0; i < VIEW_COUNT; ++i)
    {
        first[i] = Sample(simulation, VIEWS[i]);
    }
    for (uint32 frame = 0; frame < 5; ++frame)
    {
        for (usize i = VIEW_COUNT; i > 0; --i)
        {
            CHECK(AreSamplesEqual(Sample(simulation, VIEWS[i - 1]), first[i - 1]) == true);
        }
    }
    CHECK(AreBytesEqual(Save(simulation), pausedState) == true);

    // Step: one tick, its events, and the next ordinal; a twin advanced without pausing agrees.
    WeatherSimulation twin = MountGolden();
    SubmitStorm(twin);
    (void)Advance(twin, 13);
    const Array<WeatherEvent> stepEvents = Advance(simulation, 1);
    CHECK(simulation.GetTick() == 13);
    CHECK(AreBytesEqual(Save(simulation), Save(twin)) == true);
    for (const WeatherEvent& event : stepEvents)
    {
        CHECK(event.Tick == 13);
    }
}

TEST_CASE("A projected preview equals the live simulation when it reaches that tick", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    SubmitStorm(simulation);
    (void)Advance(simulation, 6);
    const WeatherSimulation scrubbed = simulation;

    constexpr uint64 HORIZON = 80;
    StaticArray<WeatherSample, VIEW_COUNT> projected{};
    for (uint64 tick = simulation.GetTick(); tick <= HORIZON; ++tick)
    {
        for (usize i = 0; i < VIEW_COUNT; ++i)
        {
            projected[i] = Preview(scrubbed, WeatherPreviewMode::Projected, tick, VIEWS[i]);
        }
        if (tick > simulation.GetTick())
        {
            (void)Advance(simulation, 1);
        }
        for (usize i = 0; i < VIEW_COUNT; ++i)
        {
            CHECK(AreSamplesEqual(projected[i], Sample(simulation, VIEWS[i])) == true);
        }
    }
}

TEST_CASE("A far projected preview jumps to the answer the climate gives once overrides end", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    SubmitStorm(simulation);
    // Every override has ended long before this tick, so only the authored schedules remain.
    constexpr uint64 FAR_TICK = 1'000'000'000'017ULL;
    for (const ViewPosition& view : VIEWS)
    {
        const WeatherSample projected = Preview(simulation, WeatherPreviewMode::Projected, FAR_TICK, view);
        const WeatherSample authored = Preview(simulation, WeatherPreviewMode::Authored, FAR_TICK, view);
        CHECK(projected.Tick == FAR_TICK);
        CHECK(projected.Overrides.IsEmpty() == true);
        CHECK(AreSamplesEqual(projected, authored) == true);
    }
    CHECK(simulation.GetTick() == 0);
}

TEST_CASE("Previews leave the live state, save and event ordinals untouched", "[weather][session]")
{
    WeatherSimulation previewed = MountGolden();
    WeatherSimulation untouched = MountGolden();
    SubmitStorm(previewed);
    SubmitStorm(untouched);
    for (uint32 step = 0; step < 50; ++step)
    {
        for (const ViewPosition& view : VIEWS)
        {
            (void)Preview(previewed, WeatherPreviewMode::Projected, previewed.GetTick() + uint64{3} * step, view);
            (void)Preview(previewed, WeatherPreviewMode::Authored, uint64{step} * 11, view);
        }
        CHECK(AreBytesEqual(Save(previewed), Save(untouched)) == true);
        const Array<WeatherEvent> previewedEvents = Advance(previewed, 1);
        const Array<WeatherEvent> untouchedEvents = Advance(untouched, 1);
        REQUIRE(previewedEvents.GetSize() == untouchedEvents.GetSize());
        for (usize i = 0; i < previewedEvents.GetSize(); ++i)
        {
            CHECK(previewedEvents[i] == untouchedEvents[i]);
        }
    }
}

TEST_CASE("Authored previews ignore overrides and reach earlier ticks; projected ones cannot", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    SubmitStorm(simulation);
    (void)Advance(simulation, 30);
    const ViewPosition core = VIEWS[2];
    REQUIRE(Sample(simulation, core).Overrides.IsEmpty() == false);

    WeatherSimulation climateOnly = MountGolden();
    (void)Advance(climateOnly, 30);
    CHECK(AreSamplesEqual(Preview(simulation, WeatherPreviewMode::Authored, 30, core), Sample(climateOnly, core)) ==
          true);
    // Earlier ticks: authored only.
    const WeatherSample earlier = Preview(simulation, WeatherPreviewMode::Authored, 3, core);
    CHECK(earlier.Tick == 3);
    CHECK(earlier.Overrides.IsEmpty() == true);
    WeatherSample untouched;
    untouched.Tick = 99;
    CHECK(simulation.TryPreview(WeatherPreviewMode::Projected, 29, core.XWorld, core.ZWorld, untouched) ==
          WeatherSimulationStatus::PreviewInPast);
    CHECK(simulation.TryPreview(WeatherPreviewMode::Authored, 29, 5000.0, 0.0, untouched) ==
          WeatherSimulationStatus::OutsideDomain);
    CHECK(simulation.TryPreview(static_cast<WeatherPreviewMode>(7), 31, core.XWorld, core.ZWorld, untouched) ==
          WeatherSimulationStatus::InvalidArgument);
    CHECK(untouched.Tick == 99);
    WeatherSimulation unmounted;
    CHECK(unmounted.TryPreview(WeatherPreviewMode::Authored, 0, 0.0, 0.0, untouched) ==
          WeatherSimulationStatus::InvalidArgument);
}

TEST_CASE("Region boundaries resolve by position, continuously, whichever view asks", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    constexpr uint32 VALLEY_REGION = 7;
    // Across the valley's western edge (-800 m) and feather (200 m), the valley weight rises monotonically
    // from zero to one and the default region takes the rest.
    ludus::foundation::int32 previous = -1;
    constexpr uint32 STEP_COUNT = 24;
    for (uint32 i = 0; i <= STEP_COUNT; ++i)
    {
        const float64 x = -850.0 + 12.5 * i;
        const WeatherSample sample = Sample(simulation, {x, 0.0});
        const ludus::foundation::int32 weight = GetRegionWeight(sample, VALLEY_REGION);
        CHECK(weight >= previous);
        CHECK(GetRegionWeight(sample, VALLEY_REGION) + GetRegionWeight(sample, DEFAULT_WEATHER_REGION_ID) ==
              format::FRACTION_ONE);
        previous = weight;
    }
    CHECK(previous == format::FRACTION_ONE);
    CHECK(GetRegionWeight(Sample(simulation, VIEWS[0]), VALLEY_REGION) == 0);
    CHECK(GetRegionWeight(Sample(simulation, VIEWS[1]), VALLEY_REGION) > 0);
    CHECK(GetRegionWeight(Sample(simulation, VIEWS[1]), VALLEY_REGION) < format::FRACTION_ONE);
    CHECK(GetRegionWeight(Sample(simulation, VIEWS[3]), VALLEY_REGION) == format::FRACTION_ONE);

    // A view inside the valley sees the outside camera's position the same way that camera does.
    SubmitStorm(simulation);
    for (uint32 tick = 0; tick < 60; ++tick)
    {
        (void)Advance(simulation, 1);
        const WeatherSample outside = Sample(simulation, VIEWS[0]);
        const WeatherSample inside = Sample(simulation, VIEWS[3]);
        CHECK(AreSamplesEqual(Sample(simulation, VIEWS[0]), outside) == true);
        CHECK(AreSamplesEqual(Sample(simulation, VIEWS[3]), inside) == true);
    }
}

TEST_CASE("Reset returns to the authored baseline and replays the same addresses", "[weather][session]")
{
    WeatherSimulation simulation = MountGolden();
    SubmitStorm(simulation);
    const Array<WeatherEvent> firstRun = Advance(simulation, 70);
    const WeatherSample firstSample = Sample(simulation, VIEWS[2]);
    REQUIRE(firstRun.IsEmpty() == false);

    simulation.ResetToAuthoredBaseline();
    CHECK(simulation.GetTick() == 0);
    CHECK(simulation.GetSeed() == 7);
    CHECK(AreBytesEqual(Save(simulation), Save(MountGolden())) == true);
    SubmitStorm(simulation);
    const Array<WeatherEvent> secondRun = Advance(simulation, 70);
    REQUIRE(secondRun.GetSize() == firstRun.GetSize());
    for (usize i = 0; i < firstRun.GetSize(); ++i)
    {
        CHECK(secondRun[i] == firstRun[i]);
    }
    CHECK(AreSamplesEqual(Sample(simulation, VIEWS[2]), firstSample) == true);
}
