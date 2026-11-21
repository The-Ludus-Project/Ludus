#include "weather_golden_corpus.h"

#include <ludus/content/weather/climate_cook.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

#include <span>

namespace ludus::gameplay::weather::testing
{
namespace
{
namespace cook = content::weather;
namespace format = content::weather_format;
using foundation::Array;
using foundation::BorrowedView;
using foundation::StaticArray;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

constexpr uint32 CLEAR = 1;
constexpr uint32 OVERCAST = 2;
constexpr uint32 RAIN = 3;
constexpr uint32 STORM = 4;
constexpr uint32 DEFAULT_SCHEDULE = 10;
constexpr uint32 VALLEY_SCHEDULE = 20;
constexpr uint32 RIDGE_SCHEDULE = 30;
constexpr uint32 VALLEY_REGION = 7;
constexpr uint32 RIDGE_REGION = 3;
constexpr uint32 STORM_OVERRIDE = 5;
constexpr uint32 CHILL_OVERRIDE = 9;
constexpr uint32 UNKNOWN_OVERRIDE = 77;
constexpr uint64 SEED = 0x5eedULL;
// The save splits the route while the interrupted storm is still blending in and commands are pending.
constexpr uint32 SAVE_TICK = 50;
// Chunk sizes sum to the comparison interval, so every route samples the same ticks.
constexpr uint32 COMPARISON_INTERVAL = 25;
constexpr usize POSITION_COUNT = 6;
constexpr usize COMMAND_COUNT = 6;
constexpr usize CHUNK_PATTERN_COUNT = 3;

struct Position final
{
    float64 XWorld;
    float64 ZWorld;
};

// Default only, valley feather, valley core, ridge over valley, ridge only, domain corner.
constexpr StaticArray<Position, POSITION_COUNT> POSITIONS{
    Position{-1500.0, 0.0},
    Position{-700.0, 0.5},
    Position{-200.0, 100.0},
    Position{400.0, 0.0},
    Position{700.0, 200.0},
    Position{2000.0, -2000.0},
};

constexpr StaticArray<uint32, CHUNK_PATTERN_COUNT> CHUNK_TICKS{5, 15, 5};

cook::WeatherPresetSource MakePreset(uint32 id,
                                     uint32 revision,
                                     float64 temperature,
                                     float64 humidity,
                                     float64 wind,
                                     float64 coverage,
                                     float64 precipitation) noexcept
{
    return
    {
        .Id = id,
        .Revision = revision,
        .TemperatureCelsius = temperature,
        .RelativeHumidity = humidity,
        .GroundWindWorld = {wind, 0.0, -0.5 * wind},
        .CloudWindWorld = {2.0 * wind, 0.25, wind},
        .CloudCoverage = coverage,
        .CloudType = 1.0 - coverage,
        .CloudDensityScale = 0.5 + coverage,
        .FogExtinctionPerMetre = 0.001 * humidity,
        .FogScaleHeightMetres = 150.0 + 100.0 * coverage,
        .PrecipitationMillimetresPerHour = precipitation,
    };
}

Array<WeatherOverrideCommand> MakeCommands(bool isReversed) noexcept
{
    const format::WeatherBoundsWorld stormFootprint
    {
        .MinimumX = -1000000,
        .MinimumZ = -1000000,
        .MaximumX = 0,
        .MaximumZ = 1000000,
    };
    const StaticArray<WeatherOverrideCommand, COMMAND_COUNT> commands{
        WeatherOverrideCommand
        {
            .OverrideId = STORM_OVERRIDE,
            .ApplyTick = 35,
            .Priority = 2,
            .PresetId = STORM,
            .ChannelGroups = static_cast<uint32>(format::WeatherChannelGroup::Clouds) |
                             static_cast<uint32>(format::WeatherChannelGroup::Precipitation) |
                             static_cast<uint32>(format::WeatherChannelGroup::GroundWind),
            .IsGlobal = false,
            .FootprintWorld = stormFootprint,
            .FootprintFeatherMillimetres = 150000,
            .BlendInTicks = 20,
            .HoldTicks = 30,
            .BlendOutTicks = 15,
        },
        WeatherOverrideCommand
        {
            .OverrideId = STORM_OVERRIDE,
            .ApplyTick = 45,
            .Priority = 2,
            .PresetId = RAIN,
            .ChannelGroups = static_cast<uint32>(format::WeatherChannelGroup::Clouds) |
                             static_cast<uint32>(format::WeatherChannelGroup::Precipitation) |
                             static_cast<uint32>(format::WeatherChannelGroup::GroundWind),
            .IsGlobal = false,
            .FootprintWorld = stormFootprint,
            .FootprintFeatherMillimetres = 150000,
            .BlendInTicks = 10,
            .HoldTicks = 0,
            .BlendOutTicks = 15,
        },
        WeatherOverrideCommand
        {
            .Action = WeatherOverrideAction::Release,
            .OverrideId = UNKNOWN_OVERRIDE,
            .ApplyTick = 50,
        },
        WeatherOverrideCommand
        {
            .Action = WeatherOverrideAction::Release,
            .OverrideId = STORM_OVERRIDE,
            .ApplyTick = 70,
            .BlendOutTicks = 12,
        },
        WeatherOverrideCommand
        {
            .OverrideId = CHILL_OVERRIDE,
            .ApplyTick = 100,
            .Priority = 9,
            .PresetId = CLEAR,
            .ChannelGroups = static_cast<uint32>(format::WeatherChannelGroup::Temperature),
            .BlendInTicks = 0,
            .HoldTicks = 20,
            .BlendOutTicks = 10,
        },
        WeatherOverrideCommand
        {
            .OverrideId = CHILL_OVERRIDE,
            .ApplyTick = 110,
            .Priority = 1,
            .PresetId = RAIN,
            .ChannelGroups = static_cast<uint32>(format::WeatherChannelGroup::Temperature),
            .BlendInTicks = 5,
        },
    };
    Array<WeatherOverrideCommand> result;
    for (usize i = 0; i < COMMAND_COUNT; ++i)
    {
        result.Add(commands[isReversed == true ? COMMAND_COUNT - 1 - i : i]);
    }
    return result;
}

class DigestWriter final
{
public:
    void WriteUnsigned(uint64 value) noexcept
    {
        constexpr usize BYTE_COUNT = sizeof(uint64);
        constexpr uint64 BYTE_MASK = 0xff;
        constexpr uint32 BITS_PER_BYTE = 8;
        for (usize i = 0; i < BYTE_COUNT; ++i)
        {
            mBytes.Add(static_cast<uint8>((value >> (i * BITS_PER_BYTE)) & BYTE_MASK));
        }
    }

    void WriteSigned(int64 value) noexcept
    {
        WriteUnsigned(static_cast<uint64>(value));
    }

    void WriteBytes(BorrowedView<const uint8> bytes) noexcept
    {
        for (const uint8 byte : bytes)
        {
            mBytes.Add(byte);
        }
    }

    void WriteEvent(const WeatherEvent& event) noexcept
    {
        WriteUnsigned(event.Tick);
        WriteUnsigned(event.Ordinal);
        WriteUnsigned(static_cast<uint64>(event.Kind));
        WriteUnsigned(event.SourceId);
        WriteUnsigned(event.Detail);
    }

    void WriteSample(const WeatherSample& sample) noexcept
    {
        WriteUnsigned(sample.Tick);
        for (const int32 code : sample.Channels.Values)
        {
            WriteSigned(code);
        }
        WriteUnsigned(sample.RegionWeights.GetSize());
        for (const WeatherRegionWeight& weight : sample.RegionWeights)
        {
            WriteUnsigned(weight.RegionId);
            WriteSigned(weight.Weight);
        }
        WriteUnsigned(sample.Overrides.GetSize());
        for (const WeatherOverrideContribution& contribution : sample.Overrides)
        {
            WriteUnsigned(contribution.OverrideId);
            WriteSigned(contribution.Weight);
        }
    }

    [[nodiscard]] foundation::Fingerprint128 GetDigest() const noexcept
    {
        return foundation::StableFingerprint128(std::span<const uint8>(mBytes.GetData(), mBytes.GetSize()));
    }

private:
    Array<uint8> mBytes;
};

bool TryMountGolden(bool isReordered, WeatherSimulation& outSimulation) noexcept
{
    Array<uint8> bytes;
    Climate climate;
    return TryCookGoldenClimate(isReordered, 1, bytes) == true &&
           TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) == format::WeatherFormatStatus::Success &&
           WeatherSimulation::TryMount(climate, GOLDEN_TICKS_PER_SECOND, SEED, outSimulation) ==
               WeatherSimulationStatus::Success;
}

bool TrySubmitAll(WeatherSimulation& inoutSimulation, bool isReversed) noexcept
{
    for (const WeatherOverrideCommand& command : MakeCommands(isReversed))
    {
        if (inoutSimulation.TrySubmit(command) != WeatherSimulationStatus::Success)
        {
            return false;
        }
    }
    return true;
}

bool TryRecordCheckpoint(const WeatherSimulation& simulation, uint32 interval, DigestWriter& inoutDigest) noexcept
{
    if (simulation.GetTick() % interval != 0)
    {
        return true;
    }
    for (const Position& position : POSITIONS)
    {
        WeatherSample sample;
        if (simulation.TrySample(position.XWorld, position.ZWorld, sample) != WeatherSimulationStatus::Success)
        {
            return false;
        }
        inoutDigest.WriteSample(sample);
    }
    return true;
}

/// Advance to endTick in chunks drawn cyclically from the pattern (one tick when isSingleStep), recording
/// every event and every checkpoint sample.
bool TryRunRoute(WeatherSimulation& inoutSimulation,
                 uint32 endTick,
                 bool isSingleStep,
                 uint32 interval,
                 DigestWriter& inoutDigest) noexcept
{
    constexpr uint32 LARGEST_CHUNK = 15;
    Array<WeatherEvent> events;
    events.Resize(usize{LARGEST_CHUNK} * MAX_WEATHER_EVENTS_PER_TICK);
    usize chunk = 0;
    while (inoutSimulation.GetTick() < endTick)
    {
        uint32 ticks = isSingleStep == true ? 1 : CHUNK_TICKS[chunk % CHUNK_PATTERN_COUNT];
        ++chunk;
        const uint64 remaining = endTick - inoutSimulation.GetTick();
        if (ticks > remaining)
        {
            ticks = static_cast<uint32>(remaining);
        }
        usize eventCount = 0;
        if (inoutSimulation.TryAdvance(ticks, {events.GetData(), events.GetSize()}, eventCount) !=
            WeatherSimulationStatus::Success)
        {
            return false;
        }
        for (usize i = 0; i < eventCount; ++i)
        {
            inoutDigest.WriteEvent(events[i]);
        }
        if (TryRecordCheckpoint(inoutSimulation, interval, inoutDigest) == false)
        {
            return false;
        }
    }
    return true;
}

bool TryAppendFinalState(const WeatherSimulation& simulation, DigestWriter& inoutDigest) noexcept
{
    Array<uint8> state;
    if (simulation.TrySaveState(state) != WeatherSimulationStatus::Success)
    {
        return false;
    }
    inoutDigest.WriteBytes({state.GetData(), state.GetSize()});
    return true;
}

/// One route: mount, submit, run in the requested way, then append the saved final state.
bool TryDigestRoute(bool isReordered,
                    bool isSingleStep,
                    bool isSplitBySave,
                    uint32 interval,
                    foundation::Fingerprint128& outDigest) noexcept
{
    WeatherSimulation simulation;
    DigestWriter digest;
    if (TryMountGolden(isReordered, simulation) == false || TrySubmitAll(simulation, isReordered) == false ||
        TryRecordCheckpoint(simulation, interval, digest) == false)
    {
        return false;
    }
    if (isSplitBySave == true)
    {
        Array<uint8> saved;
        WeatherSimulation restored;
        if (TryRunRoute(simulation, SAVE_TICK, isSingleStep, interval, digest) == false ||
            simulation.TrySaveState(saved) != WeatherSimulationStatus::Success ||
            TryMountGolden(isReordered, restored) == false ||
            restored.TryLoadState({saved.GetData(), saved.GetSize()}) != WeatherSimulationStatus::Success)
        {
            return false;
        }
        simulation = foundation::Move(restored);
    }
    if (TryRunRoute(simulation, GOLDEN_ROUTE_TICKS, isSingleStep, interval, digest) == false ||
        TryAppendFinalState(simulation, digest) == false)
    {
        return false;
    }
    outDigest = digest.GetDigest();
    return true;
}
} // namespace

bool TryCookGoldenClimate(bool isReordered, uint32 presetRevision, Array<uint8>& outBytes) noexcept
{
    constexpr usize PRESET_COUNT = 4;
    constexpr usize SCHEDULE_COUNT = 3;
    constexpr usize REGION_COUNT = 2;
    constexpr usize DEFAULT_ENTRY_COUNT = 2;
    constexpr usize VALLEY_ENTRY_COUNT = 3;
    constexpr usize RIDGE_ENTRY_COUNT = 2;
    const StaticArray<cook::WeatherPresetSource, PRESET_COUNT> presets{
        MakePreset(CLEAR, presetRevision, 22.5, 0.35, 2.0, 0.1, 0.0),
        MakePreset(OVERCAST, presetRevision, 16.0, 0.7, -4.0, 0.85, 0.0),
        MakePreset(RAIN, presetRevision, 12.25, 0.95, -7.5, 1.0, 6.0),
        MakePreset(STORM, presetRevision, 9.0, 1.0, 18.0, 1.0, 40.0),
    };
    const StaticArray<cook::WeatherScheduleEntrySource, DEFAULT_ENTRY_COUNT> defaultEntries{
        cook::WeatherScheduleEntrySource{ .PresetId = CLEAR, .HoldTicks = 40, .BlendTicks = 20 },
        cook::WeatherScheduleEntrySource{ .PresetId = OVERCAST, .HoldTicks = 30, .BlendTicks = 10 },
    };
    const StaticArray<cook::WeatherScheduleEntrySource, VALLEY_ENTRY_COUNT> valleyEntries{
        cook::WeatherScheduleEntrySource{ .PresetId = OVERCAST, .HoldTicks = 20, .BlendTicks = 10 },
        cook::WeatherScheduleEntrySource{ .PresetId = RAIN, .HoldTicks = 30, .BlendTicks = 15 },
        cook::WeatherScheduleEntrySource{ .PresetId = CLEAR, .HoldTicks = 25, .BlendTicks = 5 },
    };
    const StaticArray<cook::WeatherScheduleEntrySource, RIDGE_ENTRY_COUNT> ridgeEntries{
        cook::WeatherScheduleEntrySource{ .PresetId = OVERCAST, .HoldTicks = 15, .BlendTicks = 15 },
        cook::WeatherScheduleEntrySource{ .PresetId = CLEAR, .HoldTicks = 0, .BlendTicks = 15 },
    };
    const StaticArray<cook::WeatherScheduleSource, SCHEDULE_COUNT> schedules{
        cook::WeatherScheduleSource
        {
            .Id = DEFAULT_SCHEDULE,
            .Entries = {defaultEntries.GetData(), DEFAULT_ENTRY_COUNT},
            .LoopStartEntry = 0,
        },
        cook::WeatherScheduleSource
        {
            .Id = VALLEY_SCHEDULE,
            .Entries = {valleyEntries.GetData(), VALLEY_ENTRY_COUNT},
            .LoopStartEntry = 1,
        },
        cook::WeatherScheduleSource
        {
            .Id = RIDGE_SCHEDULE,
            .Entries = {ridgeEntries.GetData(), RIDGE_ENTRY_COUNT},
            .LoopStartEntry = 0,
        },
    };
    const StaticArray<cook::WeatherRegionSource, REGION_COUNT> regions{
        cook::WeatherRegionSource
        {
            .Id = VALLEY_REGION,
            .Priority = 1,
            .ScheduleId = VALLEY_SCHEDULE,
            .BoundsWorld = { .MinimumX = -800.0, .MinimumZ = -500.0, .MaximumX = 600.0, .MaximumZ = 700.0 },
            .FeatherMetres = 200.0,
        },
        cook::WeatherRegionSource
        {
            .Id = RIDGE_REGION,
            .Priority = 5,
            .ScheduleId = RIDGE_SCHEDULE,
            .BoundsWorld = { .MinimumX = 200.0, .MinimumZ = -300.0, .MaximumX = 900.0, .MaximumZ = 300.0 },
            .FeatherMetres = 0.0,
        },
    };
    StaticArray<cook::WeatherPresetSource, PRESET_COUNT> orderedPresets = presets;
    StaticArray<cook::WeatherScheduleSource, SCHEDULE_COUNT> orderedSchedules = schedules;
    StaticArray<cook::WeatherRegionSource, REGION_COUNT> orderedRegions = regions;
    if (isReordered == true)
    {
        for (usize i = 0; i < PRESET_COUNT; ++i)
        {
            orderedPresets[i] = presets[PRESET_COUNT - 1 - i];
        }
        for (usize i = 0; i < SCHEDULE_COUNT; ++i)
        {
            orderedSchedules[i] = schedules[SCHEDULE_COUNT - 1 - i];
        }
        for (usize i = 0; i < REGION_COUNT; ++i)
        {
            orderedRegions[i] = regions[REGION_COUNT - 1 - i];
        }
    }
    const cook::ClimateSource source
    {
        .Id = 1,
        .Revision = 1,
        .TicksPerSecond = GOLDEN_TICKS_PER_SECOND,
        .DomainWorld = { .MinimumX = -2000.0, .MinimumZ = -2000.0, .MaximumX = 2000.0, .MaximumZ = 2000.0 },
        .DefaultScheduleId = DEFAULT_SCHEDULE,
        .Presets = {orderedPresets.GetData(), PRESET_COUNT},
        .Schedules = {orderedSchedules.GetData(), SCHEDULE_COUNT},
        .Regions = {orderedRegions.GetData(), REGION_COUNT},
    };
    return cook::TryCookClimate(source, outBytes).Status == cook::ClimateCookStatus::Success;
}

bool TryComputeWeatherGoldenDigest(foundation::Fingerprint128& outDigest) noexcept
{
    Array<uint8> authored;
    Array<uint8> reordered;
    if (TryCookGoldenClimate(false, 1, authored) == false || TryCookGoldenClimate(true, 1, reordered) == false ||
        authored.GetSize() != reordered.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < authored.GetSize(); ++i)
    {
        if (authored[i] != reordered[i])
        {
            return false;
        }
    }
    // Every tick sampled on the reference route; the comparison routes sample every COMPARISON_INTERVAL ticks.
    constexpr uint32 EVERY_TICK = 1;
    foundation::Fingerprint128 golden;
    foundation::Fingerprint128 single;
    foundation::Fingerprint128 chunked;
    foundation::Fingerprint128 split;
    foundation::Fingerprint128 chunkedSplit;
    foundation::Fingerprint128 reorderedRoute;
    if (TryDigestRoute(false, true, false, EVERY_TICK, golden) == false ||
        TryDigestRoute(false, true, false, COMPARISON_INTERVAL, single) == false ||
        TryDigestRoute(false, false, false, COMPARISON_INTERVAL, chunked) == false ||
        TryDigestRoute(false, true, true, COMPARISON_INTERVAL, split) == false ||
        TryDigestRoute(false, false, true, COMPARISON_INTERVAL, chunkedSplit) == false ||
        TryDigestRoute(true, true, false, EVERY_TICK, reorderedRoute) == false)
    {
        return false;
    }
    if (chunked != single || split != single || chunkedSplit != single || reorderedRoute != golden)
    {
        return false;
    }
    outDigest = golden;
    return true;
}

bool RunWeatherGoldenCorpus() noexcept
{
    foundation::Fingerprint128 digest;
    return TryComputeWeatherGoldenDigest(digest) == true && digest == EXPECTED_WEATHER_GOLDEN_DIGEST;
}
} // namespace ludus::gameplay::weather::testing
