// Deterministic regional weather on fixed ticks.
//
// Thanks to Emily Zhou and Colin Weick, "Simulating Tropical Weather in Far Cry 6", GDC 2022, slides
// 15-29 and 32-34
// (https://media.gdcvault.com/GDC%2B2022/Speaker%2BSlides/SimulatingTropicalWeather_Weick_Colin_Zhou_Emily.pdf), for
// authored regional schedules with gameplay overrides that blend over them, and for resolving weather at positions
// rather than at the camera. Ludus adapts the idea with integer codes, priority-ordered residual weights and continuous
// interruption; no implementation is copied. Design review:
// docs/architecture/world-environment.md#authored-climate-and-resolved-weather.
#include <ludus/gameplay/weather/weather_simulation.h>

#include "internal/weather_resolve.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::gameplay::weather
{
namespace
{
namespace format = weather_format;
using format::WeatherChannel;
using format::WeatherChannelCodes;

/// Collects events in order and assigns session ordinals.
class EventCollector final
{
public:
    EventCollector(foundation::BorrowedView<WeatherEvent> outEvents, uint64& inoutNextOrdinal) noexcept
        : mOutEvents(outEvents), mNextOrdinal(inoutNextOrdinal)
    {
    }

    void Emit(uint64 tick, WeatherEventKind kind, uint32 sourceId, uint32 detail) noexcept
    {
        if (mCount == mOutEvents.GetSize())
        {
            mHasOverflowed = true;
            return;
        }
        mOutEvents[mCount] =
        {
            .Tick = tick,
            .Ordinal = mNextOrdinal,
            .Kind = kind,
            .SourceId = sourceId,
            .Detail = detail,
        };
        ++mCount;
        ++mNextOrdinal;
    }

    [[nodiscard]] LUDUS_INLINE bool HasOverflowed() const noexcept
    {
        return mHasOverflowed;
    }

    [[nodiscard]] LUDUS_INLINE usize GetCount() const noexcept
    {
        return mCount;
    }

private:
    foundation::BorrowedView<WeatherEvent> mOutEvents;
    uint64& mNextOrdinal;
    usize mCount = 0;
    bool mHasOverflowed = false;
};

bool TryFindActiveOverride(const WeatherSimulationState& state, uint32 overrideId, usize& outIndex) noexcept
{
    for (usize i = 0; i < state.ActiveOverrides.GetSize(); ++i)
    {
        if (state.ActiveOverrides[i].OverrideId == overrideId)
        {
            outIndex = i;
            return true;
        }
    }
    return false;
}

/// Apply every phase change of one override that is due at the tick. Returns whether the override ended.
bool AdvanceOverridePhase(WeatherOverrideState& inoutState, uint64 tick, EventCollector& inoutEvents) noexcept
{
    for (;;)
    {
        const uint64 elapsed = tick - inoutState.PhaseStartTick;
        switch (inoutState.Phase)
        {
            case WeatherOverridePhase::BlendIn:
                if (elapsed < inoutState.PhaseTicks)
                {
                    return false;
                }
                inoutState.Phase = WeatherOverridePhase::Hold;
                inoutState.PhaseStartTick += inoutState.PhaseTicks;
                inoutState.PhaseTicks = inoutState.HoldTicks;
                inoutState.WeightStart = format::FRACTION_ONE;
                inoutState.SourceStart = inoutState.SourceTarget;
                break;
            case WeatherOverridePhase::Hold:
                if (inoutState.PhaseTicks == 0 || elapsed < inoutState.PhaseTicks)
                {
                    return false;
                }
                inoutState.Phase = WeatherOverridePhase::BlendOut;
                inoutState.PhaseStartTick += inoutState.PhaseTicks;
                inoutState.PhaseTicks = inoutState.BlendOutTicks;
                inoutState.WeightStart = format::FRACTION_ONE;
                inoutEvents.Emit(tick, WeatherEventKind::OverrideExpired, inoutState.OverrideId, inoutState.PresetId);
                break;
            case WeatherOverridePhase::BlendOut:
                if (elapsed < inoutState.PhaseTicks)
                {
                    return false;
                }
                inoutEvents.Emit(tick, WeatherEventKind::OverrideEnded, inoutState.OverrideId, inoutState.PresetId);
                return true;
        }
    }
}

bool HasSameFootprint(const WeatherOverrideState& state, const WeatherOverrideCommand& command) noexcept
{
    if (state.Priority != command.Priority || state.ChannelGroups != command.ChannelGroups ||
        state.IsGlobal != command.IsGlobal)
    {
        return false;
    }
    return state.IsGlobal == true || (state.FootprintWorld == command.FootprintWorld &&
                                      state.FootprintFeatherMillimetres == command.FootprintFeatherMillimetres);
}

void ApplyBegin(const Climate& climate,
                const WeatherOverrideCommand& command,
                uint64 tick,
                WeatherSimulationState& inoutState,
                EventCollector& inoutEvents) noexcept
{
    uint32 presetIndex = 0;
    LUDUS_REQUIRE(climate.TryFindPresetIndex(command.PresetId, presetIndex) == true);
    const WeatherChannelCodes& target = climate.GetPresets()[presetIndex].Channels;
    usize index = 0;
    if (TryFindActiveOverride(inoutState, command.OverrideId, index) == true)
    {
        WeatherOverrideState& active = inoutState.ActiveOverrides[index];
        if (HasSameFootprint(active, command) == false)
        {
            inoutEvents.Emit(tick,
                             WeatherEventKind::OverrideRejected,
                             command.OverrideId,
                             static_cast<uint32>(WeatherOverrideRejection::FootprintMismatch));
            return;
        }
        // Carry the source's weight and values at the boundary into the new blend, before position weighting.
        const int32 weight = internal::EvaluateOverrideWeight(active, tick);
        const WeatherChannelCodes source = internal::EvaluateOverrideSource(active, tick);
        active.PresetId = command.PresetId;
        active.Phase = WeatherOverridePhase::BlendIn;
        active.PhaseStartTick = tick;
        active.PhaseTicks = command.BlendInTicks;
        active.HoldTicks = command.HoldTicks;
        active.BlendOutTicks = command.BlendOutTicks;
        active.WeightStart = weight;
        active.SourceStart = source;
        active.SourceTarget = target;
        inoutEvents.Emit(tick, WeatherEventKind::OverrideInterrupted, command.OverrideId, command.PresetId);
        LUDUS_REQUIRE(AdvanceOverridePhase(active, tick, inoutEvents) == false);
        return;
    }
    if (inoutState.ActiveOverrides.IsFull() == true)
    {
        inoutEvents.Emit(tick,
                         WeatherEventKind::OverrideRejected,
                         command.OverrideId,
                         static_cast<uint32>(WeatherOverrideRejection::CapacityExceeded));
        return;
    }
    WeatherOverrideState created
    {
        .OverrideId = command.OverrideId,
        .Priority = command.Priority,
        .PresetId = command.PresetId,
        .ChannelGroups = command.ChannelGroups,
        .IsGlobal = command.IsGlobal,
        .FootprintWorld = command.IsGlobal == true ? format::WeatherBoundsWorld{} : command.FootprintWorld,
        .FootprintFeatherMillimetres = command.IsGlobal == true ? 0 : command.FootprintFeatherMillimetres,
        .Phase = WeatherOverridePhase::BlendIn,
        .PhaseStartTick = tick,
        .PhaseTicks = command.BlendInTicks,
        .HoldTicks = command.HoldTicks,
        .BlendOutTicks = command.BlendOutTicks,
        .WeightStart = 0,
        .SourceStart = target,
        .SourceTarget = target,
    };
    inoutEvents.Emit(tick, WeatherEventKind::OverrideStarted, command.OverrideId, command.PresetId);
    LUDUS_REQUIRE(AdvanceOverridePhase(created, tick, inoutEvents) == false);
    usize insertAt = 0;
    while (insertAt < inoutState.ActiveOverrides.GetSize() &&
           inoutState.ActiveOverrides[insertAt].OverrideId < command.OverrideId)
    {
        ++insertAt;
    }
    LUDUS_REQUIRE(inoutState.ActiveOverrides.TryInsertAt(insertAt, created) != nullptr);
}

void ApplyRelease(const WeatherOverrideCommand& command,
                  uint64 tick,
                  WeatherSimulationState& inoutState,
                  EventCollector& inoutEvents) noexcept
{
    usize index = 0;
    if (TryFindActiveOverride(inoutState, command.OverrideId, index) == false ||
        inoutState.ActiveOverrides[index].Phase == WeatherOverridePhase::BlendOut)
    {
        inoutEvents.Emit(tick,
                         WeatherEventKind::OverrideRejected,
                         command.OverrideId,
                         static_cast<uint32>(WeatherOverrideRejection::NotActive));
        return;
    }
    WeatherOverrideState& active = inoutState.ActiveOverrides[index];
    const int32 weight = internal::EvaluateOverrideWeight(active, tick);
    const WeatherChannelCodes source = internal::EvaluateOverrideSource(active, tick);
    active.Phase = WeatherOverridePhase::BlendOut;
    active.PhaseStartTick = tick;
    active.PhaseTicks = command.BlendOutTicks;
    active.WeightStart = weight;
    active.SourceStart = source;
    active.SourceTarget = source;
    inoutEvents.Emit(tick, WeatherEventKind::OverrideReleased, command.OverrideId, active.PresetId);
    if (AdvanceOverridePhase(active, tick, inoutEvents) == true)
    {
        inoutState.ActiveOverrides.RemoveAt(index);
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the default region has no record to pass instead.
void EmitScheduleEvents(const Climate& climate,
                        uint32 regionId,
                        uint32 scheduleIndex,
                        uint64 tick,
                        EventCollector& inoutEvents) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const internal::SchedulePosition position = internal::LocateSchedule(climate, scheduleIndex, tick);
    const format::WeatherScheduleRecord& schedule = climate.GetSchedules()[scheduleIndex];
    const format::WeatherScheduleEntryRecord& entry =
        climate.GetScheduleEntries()[schedule.FirstEntry + position.Entry];
    if (position.Offset == 0)
    {
        inoutEvents.Emit(tick, WeatherEventKind::ScheduleEntryBegan, regionId, position.Entry);
    }
    else if (entry.HoldTicks != 0 && entry.BlendTicks != 0 && position.Offset == entry.HoldTicks)
    {
        inoutEvents.Emit(tick, WeatherEventKind::ScheduleBlendBegan, regionId, position.Entry);
    }
}

/// Advance the state by exactly one tick.
void StepTick(const Climate& climate, WeatherSimulationState& inoutState, EventCollector& inoutEvents) noexcept
{
    const uint64 tick = ++inoutState.Tick;

    // 1. Phase changes that complete at this tick, in ascending override ID order.
    usize index = 0;
    while (index < inoutState.ActiveOverrides.GetSize())
    {
        if (AdvanceOverridePhase(inoutState.ActiveOverrides[index], tick, inoutEvents) == true)
        {
            inoutState.ActiveOverrides.RemoveAt(index);
        }
        else
        {
            ++index;
        }
    }

    // 2. Commands due at this tick, in (OverrideId, Action) order. Submission guarantees no earlier ticks remain.
    while (inoutState.PendingCommands.IsEmpty() == false && inoutState.PendingCommands[0].ApplyTick == tick)
    {
        const WeatherOverrideCommand command = inoutState.PendingCommands[0];
        inoutState.PendingCommands.RemoveAt(0);
        if (command.Action == WeatherOverrideAction::Begin)
        {
            ApplyBegin(climate, command, tick, inoutState, inoutEvents);
        }
        else
        {
            ApplyRelease(command, tick, inoutState, inoutEvents);
        }
    }

    // 3. Schedule boundaries: the default region, then regions in ascending ID order.
    EmitScheduleEvents(climate, DEFAULT_WEATHER_REGION_ID, climate.GetHeader().DefaultScheduleIndex, tick, inoutEvents);
    const foundation::BorrowedView<const format::WeatherRegionRecord> regions = climate.GetRegions();
    uint32 previousId = DEFAULT_WEATHER_REGION_ID;
    for (usize emitted = 0; emitted < regions.GetSize(); ++emitted)
    {
        usize next = regions.GetSize();
        for (usize i = 0; i < regions.GetSize(); ++i)
        {
            if (regions[i].Id > previousId && (next == regions.GetSize() || regions[i].Id < regions[next].Id))
            {
                next = i;
            }
        }
        EmitScheduleEvents(climate, regions[next].Id, regions[next].ScheduleIndex, tick, inoutEvents);
        previousId = regions[next].Id;
    }
}

bool TryQuantizePosition(float64 metres, int64 minimum, int64 maximum, int64& outCode) noexcept
{
    return format::TryQuantizeWeatherCode(metres, format::LENGTH_CODES_PER_METRE, minimum, maximum, outCode);
}
} // namespace

WeatherConditions ConvertWeatherChannels(const format::WeatherChannelCodes& channels) noexcept
{
    const float64 fraction = format::FRACTION_ONE;
    const float64 length = format::LENGTH_CODES_PER_METRE;
    const auto get = [&channels](WeatherChannel channel) noexcept {
        return static_cast<float64>(channels.Get(channel));
    };
    return
    {
        .TemperatureCelsius = get(WeatherChannel::Temperature) / format::TEMPERATURE_CODES_PER_DEGREE,
        .RelativeHumidity = get(WeatherChannel::RelativeHumidity) / fraction,
        .GroundWindWorld =
            {
                get(WeatherChannel::GroundWindX) / length,
                get(WeatherChannel::GroundWindY) / length,
                get(WeatherChannel::GroundWindZ) / length,
            },
        .CloudWindWorld =
            {
                get(WeatherChannel::CloudWindX) / length,
                get(WeatherChannel::CloudWindY) / length,
                get(WeatherChannel::CloudWindZ) / length,
            },
        .CloudCoverage = get(WeatherChannel::CloudCoverage) / fraction,
        .CloudType = get(WeatherChannel::CloudType) / fraction,
        .CloudDensityScale = get(WeatherChannel::CloudDensityScale) / fraction,
        .FogExtinctionPerMetre = get(WeatherChannel::FogExtinction) / format::FOG_EXTINCTION_CODES_PER_INVERSE_METRE,
        .FogScaleHeightMetres = get(WeatherChannel::FogScaleHeight) / length,
        .PrecipitationMillimetresPerHour =
            get(WeatherChannel::Precipitation) / format::PRECIPITATION_CODES_PER_MILLIMETRE_PER_HOUR,
    };
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a tick rate and a 64-bit seed with distinct widths.
WeatherSimulationStatus WeatherSimulation::TryMount(const Climate& climate,
                                                    uint32 sessionTicksPerSecond,
                                                    uint64 seed,
                                                    WeatherSimulation& outSimulation) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (climate.IsEmpty() == true)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    if (climate.GetHeader().TicksPerSecond != sessionTicksPerSecond)
    {
        return WeatherSimulationStatus::TickRateMismatch;
    }
    outSimulation.mClimate = climate;
    outSimulation.mSeed = seed;
    outSimulation.mState = {};
    return WeatherSimulationStatus::Success;
}

WeatherSimulationStatus WeatherSimulation::TrySubmit(const WeatherOverrideCommand& command) noexcept
{
    if (IsMounted() == false)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    const WeatherSimulationStatus validation = internal::ValidateCommand(mClimate, command);
    if (validation != WeatherSimulationStatus::Success)
    {
        return validation;
    }
    if (command.ApplyTick <= mState.Tick)
    {
        return WeatherSimulationStatus::CommandInPast;
    }
    // Keep only the fields the action reads, so saved commands have one canonical form.
    WeatherOverrideCommand accepted = command;
    if (accepted.Action == WeatherOverrideAction::Release)
    {
        accepted =
        {
            .Action = WeatherOverrideAction::Release,
            .OverrideId = command.OverrideId,
            .ApplyTick = command.ApplyTick,
            .BlendOutTicks = command.BlendOutTicks,
        };
    }
    else if (accepted.IsGlobal == true)
    {
        accepted.FootprintWorld = {};
        accepted.FootprintFeatherMillimetres = 0;
    }
    usize insertAt = 0;
    for (const WeatherOverrideCommand& pending : mState.PendingCommands)
    {
        if (pending.ApplyTick == accepted.ApplyTick && pending.OverrideId == accepted.OverrideId &&
            pending.Action == accepted.Action)
        {
            return WeatherSimulationStatus::DuplicateCommand;
        }
        if (internal::IsCommandBefore(pending, accepted) == true)
        {
            ++insertAt;
        }
    }
    if (mState.PendingCommands.IsFull() == true)
    {
        return WeatherSimulationStatus::CapacityExceeded;
    }
    LUDUS_REQUIRE(mState.PendingCommands.TryInsertAt(insertAt, accepted) != nullptr);
    return WeatherSimulationStatus::Success;
}

WeatherSimulationStatus WeatherSimulation::TryAdvance(uint32 tickCount,
                                                      foundation::BorrowedView<WeatherEvent> outEvents,
                                                      usize& outEventCount) noexcept
{
    if (IsMounted() == false || outEvents.IsValid() == false)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    uint64 endTick = 0;
    if (foundation::TryAdd(mState.Tick, uint64{tickCount}, endTick) == false)
    {
        return WeatherSimulationStatus::TickOverflow;
    }
    // Step a copy so that an event overflow leaves the simulation unchanged.
    WeatherSimulationState state = mState;
    EventCollector events(outEvents, state.NextEventOrdinal);
    while (state.Tick < endTick)
    {
        StepTick(mClimate, state, events);
        if (events.HasOverflowed() == true)
        {
            return WeatherSimulationStatus::CapacityExceeded;
        }
    }
    mState = foundation::Move(state);
    outEventCount = events.GetCount();
    return WeatherSimulationStatus::Success;
}

WeatherSimulationStatus
WeatherSimulation::TrySample(float64 positionXWorld, float64 positionZWorld, WeatherSample& outSample) const noexcept
{
    if (IsMounted() == false)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    const format::WeatherBoundsWorld& domain = mClimate.GetHeader().DomainWorld;
    int64 x = 0;
    int64 z = 0;
    if (TryQuantizePosition(positionXWorld, domain.MinimumX, domain.MaximumX, x) == false ||
        TryQuantizePosition(positionZWorld, domain.MinimumZ, domain.MaximumZ, z) == false)
    {
        return WeatherSimulationStatus::OutsideDomain;
    }
    const uint64 tick = mState.Tick;
    WeatherSample sample;
    sample.Tick = tick;

    // Regional weights: each region in resolution order claims its coverage of the weight still available,
    // and the default region takes the remainder, so the weights sum to exactly FRACTION_ONE.
    foundation::StaticArray<int64, format::WEATHER_CHANNEL_COUNT> scheduleSums{};
    int32 remaining = format::FRACTION_ONE;
    const auto addSchedule = [this, tick, &scheduleSums](uint32 scheduleIndex, int32 weight) noexcept {
        const WeatherChannelCodes values = internal::EvaluateSchedule(mClimate, scheduleIndex, tick);
        for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
        {
            scheduleSums[i] += int64{weight} * values.Values[i];
        }
    };
    for (const format::WeatherRegionRecord& region : mClimate.GetRegions())
    {
        const int32 coverage = internal::ComputeFeatheredCoverage(region.BoundsWorld, region.FeatherMillimetres, x, z);
        const int32 weight = internal::MultiplyFractions(coverage, remaining);
        if (weight == 0)
        {
            continue;
        }
        remaining -= weight;
        addSchedule(region.ScheduleIndex, weight);
        LUDUS_REQUIRE(sample.RegionWeights.TryAdd({ .RegionId = region.Id, .Weight = weight }) == true);
    }
    if (remaining != 0)
    {
        addSchedule(mClimate.GetHeader().DefaultScheduleIndex, remaining);
        LUDUS_REQUIRE(sample.RegionWeights.TryAdd({ .RegionId = DEFAULT_WEATHER_REGION_ID, .Weight = remaining }) ==
                      true);
    }
    WeatherChannelCodes underlying;
    for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
    {
        underlying.Values[i] = static_cast<int32>(internal::DivideRounded(scheduleSums[i], format::FRACTION_ONE));
    }

    // Overrides in resolution order claim, per channel, their weight of what higher overrides left;
    // the running schedule fills the residual.
    foundation::FixedArray<const WeatherOverrideState*, MAX_ACTIVE_WEATHER_OVERRIDES> ordered;
    for (const WeatherOverrideState& active : mState.ActiveOverrides)
    {
        LUDUS_REQUIRE(ordered.TryAdd(&active) == true);
    }
    foundation::Sort(ordered.GetData(),
                     ordered.GetSize(),
                     [](const WeatherOverrideState* left, const WeatherOverrideState* right) {
                         return internal::IsOverrideBefore(*left, *right);
                     });
    foundation::StaticArray<int64, format::WEATHER_CHANNEL_COUNT> overrideSums{};
    foundation::StaticArray<int32, format::WEATHER_CHANNEL_COUNT> residual{};
    for (int32& value : residual)
    {
        value = format::FRACTION_ONE;
    }
    for (const WeatherOverrideState* active : ordered)
    {
        const int32 coverage =
            active->IsGlobal == true
                ? format::FRACTION_ONE
                : internal::ComputeFeatheredCoverage(active->FootprintWorld, active->FootprintFeatherMillimetres, x, z);
        const int32 weight = internal::MultiplyFractions(coverage, internal::EvaluateOverrideWeight(*active, tick));
        if (weight == 0)
        {
            continue;
        }
        LUDUS_REQUIRE(sample.Overrides.TryAdd({ .OverrideId = active->OverrideId, .Weight = weight }) == true);
        const WeatherChannelCodes source = internal::EvaluateOverrideSource(*active, tick);
        for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
        {
            const uint32 group = static_cast<uint32>(format::GetWeatherChannelGroup(static_cast<WeatherChannel>(i)));
            if ((active->ChannelGroups & group) == 0)
            {
                continue;
            }
            const int32 channelWeight = internal::MultiplyFractions(weight, residual[i]);
            overrideSums[i] += int64{channelWeight} * source.Values[i];
            residual[i] -= channelWeight;
        }
    }
    for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
    {
        const int64 sum = overrideSums[i] + int64{residual[i]} * underlying.Values[i];
        sample.Channels.Values[i] = static_cast<int32>(internal::DivideRounded(sum, format::FRACTION_ONE));
    }
    outSample = foundation::Move(sample);
    return WeatherSimulationStatus::Success;
}

void WeatherSimulation::ResetToAuthoredBaseline() noexcept
{
    mState = {};
}
} // namespace ludus::gameplay::weather
