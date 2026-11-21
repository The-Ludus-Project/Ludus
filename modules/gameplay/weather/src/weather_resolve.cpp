#include "internal/weather_resolve.h"

namespace ludus::gameplay::weather::internal
{
namespace format = weather_format;

namespace
{
constexpr int64 FRACTION_ONE = format::FRACTION_ONE;
/// Fraction bits of a fraction code.
constexpr uint32 FRACTION_BITS = 16;
/// Smoothstep is 3t^2 - 2t^3.
constexpr int64 SMOOTHSTEP_QUADRATIC_FACTOR = 3;

int64 GetMinimum(int64 left, int64 right) noexcept
{
    return left < right ? left : right;
}

uint64 GetEntryTicks(const format::WeatherScheduleEntryRecord& entry) noexcept
{
    return uint64{entry.HoldTicks} + uint64{entry.BlendTicks};
}

uint64 GetElapsedPhaseTicks(const WeatherOverrideState& state, uint64 tick) noexcept
{
    const uint64 elapsed = tick > state.PhaseStartTick ? tick - state.PhaseStartTick : 0;
    return elapsed < state.PhaseTicks ? elapsed : state.PhaseTicks;
}
} // namespace

int64 DivideRounded(int64 numerator, int64 denominator) noexcept
{
    LUDUS_ASSERT(denominator > 0);
    const int64 half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator : -((half - numerator) / denominator);
}

int32 InterpolateCode(int32 start, int32 end, uint64 numerator, uint64 denominator) noexcept
{
    if (denominator == 0 || numerator >= denominator)
    {
        return end;
    }
    // |end - start| < 2^31 and denominator <= 2^32, so the product fits int64.
    const int64 difference = int64{end} - int64{start};
    const int64 step = DivideRounded(difference * static_cast<int64>(numerator), static_cast<int64>(denominator));
    return static_cast<int32>(int64{start} + step);
}

format::WeatherChannelCodes InterpolateChannels(const format::WeatherChannelCodes& start,
                                                const format::WeatherChannelCodes& end,
                                                uint64 numerator,
                                                uint64 denominator) noexcept
{
    format::WeatherChannelCodes result;
    for (usize i = 0; i < format::WEATHER_CHANNEL_COUNT; ++i)
    {
        result.Values[i] = InterpolateCode(start.Values[i], end.Values[i], numerator, denominator);
    }
    return result;
}

int32 MultiplyFractions(int32 left, int32 right) noexcept
{
    const int64 product = int64{left} * int64{right};
    return static_cast<int32>((product + (FRACTION_ONE / 2)) >> FRACTION_BITS);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a World X/Z coordinate pair in axis order.
int32 ComputeFeatheredCoverage(const format::WeatherBoundsWorld& bounds,
                               uint32 featherMillimetres,
                               int64 positionXWorld,
                               int64 positionZWorld) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (positionXWorld <= bounds.MinimumX || positionXWorld >= bounds.MaximumX || positionZWorld <= bounds.MinimumZ ||
        positionZWorld >= bounds.MaximumZ)
    {
        return 0;
    }
    const int64 depth = GetMinimum(GetMinimum(positionXWorld - bounds.MinimumX, bounds.MaximumX - positionXWorld),
                                   GetMinimum(positionZWorld - bounds.MinimumZ, bounds.MaximumZ - positionZWorld));
    const int64 feather = featherMillimetres;
    if (feather == 0 || depth >= feather)
    {
        return format::FRACTION_ONE;
    }
    // t in [0, FRACTION_ONE); t^2 (3 - 2t) in fraction units, rounded once.
    const int64 t = depth * FRACTION_ONE / feather;
    const int64 numerator = t * t * (SMOOTHSTEP_QUADRATIC_FACTOR * FRACTION_ONE - 2 * t);
    return static_cast<int32>(DivideRounded(numerator, FRACTION_ONE * FRACTION_ONE));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a table index and a session tick have distinct widths.
SchedulePosition LocateSchedule(const Climate& climate, uint32 scheduleIndex, uint64 tick) noexcept
{
    const format::WeatherScheduleRecord& schedule = climate.GetSchedules()[scheduleIndex];
    const foundation::BorrowedView<const format::WeatherScheduleEntryRecord> entries = climate.GetScheduleEntries();
    uint64 firstPassTicks = 0;
    uint64 loopOffsetTicks = 0;
    for (uint32 i = 0; i < schedule.EntryCount; ++i)
    {
        if (i == schedule.LoopStartEntry)
        {
            loopOffsetTicks = firstPassTicks;
        }
        firstPassTicks += GetEntryTicks(entries[schedule.FirstEntry + i]);
    }
    // Every entry lasts at least one tick, so the loop is never empty.
    const uint64 loopTicks = firstPassTicks - loopOffsetTicks;
    const uint64 passTick = tick < firstPassTicks ? tick : loopOffsetTicks + (tick - firstPassTicks) % loopTicks;
    uint64 entryStart = 0;
    for (uint32 i = 0; i < schedule.EntryCount; ++i)
    {
        const uint64 entryTicks = GetEntryTicks(entries[schedule.FirstEntry + i]);
        if (passTick < entryStart + entryTicks)
        {
            return { .Entry = i, .Offset = passTick - entryStart };
        }
        entryStart += entryTicks;
    }
    LUDUS_UNREACHABLE();
}

format::WeatherChannelCodes EvaluateSchedule(const Climate& climate, uint32 scheduleIndex, uint64 tick) noexcept
{
    const format::WeatherScheduleRecord& schedule = climate.GetSchedules()[scheduleIndex];
    const foundation::BorrowedView<const format::WeatherScheduleEntryRecord> entries = climate.GetScheduleEntries();
    const foundation::BorrowedView<const format::WeatherPresetRecord> presets = climate.GetPresets();
    const SchedulePosition position = LocateSchedule(climate, scheduleIndex, tick);
    const format::WeatherScheduleEntryRecord& entry = entries[schedule.FirstEntry + position.Entry];
    const format::WeatherChannelCodes& held = presets[entry.PresetIndex].Channels;
    if (position.Offset < entry.HoldTicks)
    {
        return held;
    }
    const uint32 nextEntry = position.Entry + 1 < schedule.EntryCount ? position.Entry + 1 : schedule.LoopStartEntry;
    const format::WeatherChannelCodes& next = presets[entries[schedule.FirstEntry + nextEntry].PresetIndex].Channels;
    return InterpolateChannels(held, next, position.Offset - entry.HoldTicks, entry.BlendTicks);
}

int32 EvaluateOverrideWeight(const WeatherOverrideState& state, uint64 tick) noexcept
{
    switch (state.Phase)
    {
        case WeatherOverridePhase::BlendIn:
            return InterpolateCode(state.WeightStart,
                                   format::FRACTION_ONE,
                                   GetElapsedPhaseTicks(state, tick),
                                   state.PhaseTicks);
        case WeatherOverridePhase::Hold:
            return format::FRACTION_ONE;
        case WeatherOverridePhase::BlendOut:
            return InterpolateCode(state.WeightStart, 0, GetElapsedPhaseTicks(state, tick), state.PhaseTicks);
    }
    return 0;
}

format::WeatherChannelCodes EvaluateOverrideSource(const WeatherOverrideState& state, uint64 tick) noexcept
{
    if (state.Phase != WeatherOverridePhase::BlendIn)
    {
        return state.SourceTarget;
    }
    return InterpolateChannels(state.SourceStart,
                               state.SourceTarget,
                               GetElapsedPhaseTicks(state, tick),
                               state.PhaseTicks);
}

bool IsCommandBefore(const WeatherOverrideCommand& left, const WeatherOverrideCommand& right) noexcept
{
    if (left.ApplyTick != right.ApplyTick)
    {
        return left.ApplyTick < right.ApplyTick;
    }
    if (left.OverrideId != right.OverrideId)
    {
        return left.OverrideId < right.OverrideId;
    }
    return static_cast<uint8>(left.Action) < static_cast<uint8>(right.Action);
}

bool IsOverrideBefore(const WeatherOverrideState& left, const WeatherOverrideState& right) noexcept
{
    return left.Priority != right.Priority ? left.Priority > right.Priority : left.OverrideId < right.OverrideId;
}

WeatherSimulationStatus ValidateCommand(const Climate& climate, const WeatherOverrideCommand& command) noexcept
{
    if (command.OverrideId == 0 ||
        (command.Action != WeatherOverrideAction::Begin && command.Action != WeatherOverrideAction::Release))
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    if (command.Action == WeatherOverrideAction::Release)
    {
        return WeatherSimulationStatus::Success;
    }
    uint32 presetIndex = 0;
    if (climate.TryFindPresetIndex(command.PresetId, presetIndex) == false)
    {
        return WeatherSimulationStatus::UnknownPreset;
    }
    if (command.ChannelGroups == 0 || (command.ChannelGroups & ~format::ALL_WEATHER_CHANNEL_GROUPS) != 0)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    if (command.IsGlobal == false &&
        (format::IsWeatherBoundsValid(command.FootprintWorld) == false ||
         format::IsWeatherBoundsContained(command.FootprintWorld, climate.GetHeader().DomainWorld) == false))
    {
        return WeatherSimulationStatus::InvalidFootprint;
    }
    return WeatherSimulationStatus::Success;
}
} // namespace ludus::gameplay::weather::internal
