// Saved weather state (LUWST). Layout, little-endian with no padding:
//
//   Magic[8] "LUWST01\0"; StateVersion; ClimateId; ClimateRevision; TicksPerSecond (uint32 each);
//   ClimateFingerprint Low, High; Seed; Tick; NextEventOrdinal (uint64 each);
//   ActiveCount; PendingCount (uint32 each); ActiveCount override records; PendingCount command records.
//
// The climate identity covers every preset revision, because the fingerprint is taken over the exact cooked
// blob. A mismatch is IncompatibleClimate, whose repair is WeatherSimulation::ResetToAuthoredBaseline.
#include <ludus/gameplay/weather/weather_simulation.h>

#include "internal/weather_resolve.h"

#include <span>
#include <string_view>

namespace ludus::gameplay::weather
{
namespace
{
namespace format = weather_format;

constexpr std::string_view STATE_MAGIC{"LUWST01\0", 8};
/// Version 1: tick, ordinal, active overrides and pending commands.
constexpr uint32 STATE_VERSION_1 = 1;
constexpr uint32 STATE_VERSION = STATE_VERSION_1;
constexpr uint32 GLOBAL_FOOTPRINT_FLAG = 1;

constexpr usize BOUNDS_BYTES = 4 * sizeof(int64);
constexpr usize CHANNELS_BYTES = format::WEATHER_CHANNEL_COUNT * sizeof(int32);
constexpr usize STATE_HEADER_BYTES = STATE_MAGIC.size() + 4 * sizeof(uint32) + 5 * sizeof(uint64) + 2 * sizeof(uint32);
constexpr usize OVERRIDE_RECORD_BYTES = 11 * sizeof(uint32) + sizeof(uint64) + BOUNDS_BYTES + 2 * CHANNELS_BYTES;
constexpr usize COMMAND_RECORD_BYTES = 10 * sizeof(uint32) + sizeof(uint64) + BOUNDS_BYTES;

void WriteOverride(format::WeatherByteWriter& inoutWriter, const WeatherOverrideState& state) noexcept
{
    inoutWriter.WriteUnsigned(state.OverrideId);
    inoutWriter.WriteInt32(state.Priority);
    inoutWriter.WriteUnsigned(state.PresetId);
    inoutWriter.WriteUnsigned(state.ChannelGroups);
    inoutWriter.WriteUnsigned(state.IsGlobal == true ? GLOBAL_FOOTPRINT_FLAG : 0U);
    format::WriteWeatherBounds(inoutWriter, state.FootprintWorld);
    inoutWriter.WriteUnsigned(state.FootprintFeatherMillimetres);
    inoutWriter.WriteUnsigned(static_cast<uint32>(state.Phase));
    inoutWriter.WriteUnsigned(state.PhaseStartTick);
    inoutWriter.WriteUnsigned(state.PhaseTicks);
    inoutWriter.WriteUnsigned(state.HoldTicks);
    inoutWriter.WriteUnsigned(state.BlendOutTicks);
    inoutWriter.WriteInt32(state.WeightStart);
    format::WriteWeatherChannels(inoutWriter, state.SourceStart);
    format::WriteWeatherChannels(inoutWriter, state.SourceTarget);
}

void WriteCommand(format::WeatherByteWriter& inoutWriter, const WeatherOverrideCommand& command) noexcept
{
    inoutWriter.WriteUnsigned(static_cast<uint32>(command.Action));
    inoutWriter.WriteUnsigned(command.OverrideId);
    inoutWriter.WriteUnsigned(command.ApplyTick);
    inoutWriter.WriteInt32(command.Priority);
    inoutWriter.WriteUnsigned(command.PresetId);
    inoutWriter.WriteUnsigned(command.ChannelGroups);
    inoutWriter.WriteUnsigned(command.IsGlobal == true ? GLOBAL_FOOTPRINT_FLAG : 0U);
    format::WriteWeatherBounds(inoutWriter, command.FootprintWorld);
    inoutWriter.WriteUnsigned(command.FootprintFeatherMillimetres);
    inoutWriter.WriteUnsigned(command.BlendInTicks);
    inoutWriter.WriteUnsigned(command.HoldTicks);
    inoutWriter.WriteUnsigned(command.BlendOutTicks);
}

bool TryReadFlag(format::WeatherByteReader& inoutReader, bool& outValue) noexcept
{
    const uint32 flag = inoutReader.ReadUnsigned<uint32>();
    if (flag != 0 && flag != GLOBAL_FOOTPRINT_FLAG)
    {
        return false;
    }
    outValue = flag == GLOBAL_FOOTPRINT_FLAG;
    return true;
}

bool TryReadOverride(format::WeatherByteReader& inoutReader, WeatherOverrideState& outState) noexcept
{
    WeatherOverrideState state;
    state.OverrideId = inoutReader.ReadUnsigned<uint32>();
    state.Priority = inoutReader.ReadInt32();
    state.PresetId = inoutReader.ReadUnsigned<uint32>();
    state.ChannelGroups = inoutReader.ReadUnsigned<uint32>();
    const bool isFlagValid = TryReadFlag(inoutReader, state.IsGlobal);
    state.FootprintWorld = format::ReadWeatherBounds(inoutReader);
    state.FootprintFeatherMillimetres = inoutReader.ReadUnsigned<uint32>();
    const uint32 phase = inoutReader.ReadUnsigned<uint32>();
    state.PhaseStartTick = inoutReader.ReadUnsigned<uint64>();
    state.PhaseTicks = inoutReader.ReadUnsigned<uint32>();
    state.HoldTicks = inoutReader.ReadUnsigned<uint32>();
    state.BlendOutTicks = inoutReader.ReadUnsigned<uint32>();
    state.WeightStart = inoutReader.ReadInt32();
    state.SourceStart = format::ReadWeatherChannels(inoutReader);
    state.SourceTarget = format::ReadWeatherChannels(inoutReader);
    if (isFlagValid == false || phase > static_cast<uint32>(WeatherOverridePhase::BlendOut))
    {
        return false;
    }
    state.Phase = static_cast<WeatherOverridePhase>(phase);
    outState = state;
    return true;
}

bool TryReadCommand(format::WeatherByteReader& inoutReader, WeatherOverrideCommand& outCommand) noexcept
{
    WeatherOverrideCommand command;
    const uint32 action = inoutReader.ReadUnsigned<uint32>();
    command.OverrideId = inoutReader.ReadUnsigned<uint32>();
    command.ApplyTick = inoutReader.ReadUnsigned<uint64>();
    command.Priority = inoutReader.ReadInt32();
    command.PresetId = inoutReader.ReadUnsigned<uint32>();
    command.ChannelGroups = inoutReader.ReadUnsigned<uint32>();
    const bool isFlagValid = TryReadFlag(inoutReader, command.IsGlobal);
    command.FootprintWorld = format::ReadWeatherBounds(inoutReader);
    command.FootprintFeatherMillimetres = inoutReader.ReadUnsigned<uint32>();
    command.BlendInTicks = inoutReader.ReadUnsigned<uint32>();
    command.HoldTicks = inoutReader.ReadUnsigned<uint32>();
    command.BlendOutTicks = inoutReader.ReadUnsigned<uint32>();
    if (isFlagValid == false || action > static_cast<uint32>(WeatherOverrideAction::Release))
    {
        return false;
    }
    command.Action = static_cast<WeatherOverrideAction>(action);
    outCommand = command;
    return true;
}

/// Whether an active override is consistent with the climate and the saved tick: a phase that would
/// already have changed at that tick cannot come from TrySaveState.
bool IsOverrideStateValid(const Climate& climate, const WeatherOverrideState& state, uint64 tick) noexcept
{
    const WeatherOverrideCommand asCommand
    {
        .OverrideId = state.OverrideId,
        .Priority = state.Priority,
        .PresetId = state.PresetId,
        .ChannelGroups = state.ChannelGroups,
        .IsGlobal = state.IsGlobal,
        .FootprintWorld = state.FootprintWorld,
        .FootprintFeatherMillimetres = state.FootprintFeatherMillimetres,
    };
    if (internal::ValidateCommand(climate, asCommand) != WeatherSimulationStatus::Success ||
        (state.IsGlobal == true &&
         (state.FootprintWorld != format::WeatherBoundsWorld{} || state.FootprintFeatherMillimetres != 0)) ||
        state.PhaseStartTick > tick || state.WeightStart < 0 || state.WeightStart > format::FRACTION_ONE ||
        state.SourceStart.IsValid() == false || state.SourceTarget.IsValid() == false)
    {
        return false;
    }
    const uint64 elapsed = tick - state.PhaseStartTick;
    switch (state.Phase)
    {
        case WeatherOverridePhase::BlendIn:
        case WeatherOverridePhase::BlendOut:
            return elapsed < state.PhaseTicks;
        case WeatherOverridePhase::Hold:
            return state.PhaseTicks == state.HoldTicks && (state.PhaseTicks == 0 || elapsed < state.PhaseTicks) &&
                   state.WeightStart == format::FRACTION_ONE;
    }
    return false;
}

/// Whether a pending command is in the canonical form TrySubmit stores.
bool IsCommandCanonical(const WeatherOverrideCommand& command) noexcept
{
    if (command.Action == WeatherOverrideAction::Release)
    {
        const WeatherOverrideCommand canonical
        {
            .Action = WeatherOverrideAction::Release,
            .OverrideId = command.OverrideId,
            .ApplyTick = command.ApplyTick,
            .BlendOutTicks = command.BlendOutTicks,
        };
        return command.Priority == canonical.Priority && command.PresetId == canonical.PresetId &&
               command.ChannelGroups == canonical.ChannelGroups && command.IsGlobal == canonical.IsGlobal &&
               command.FootprintWorld == canonical.FootprintWorld &&
               command.FootprintFeatherMillimetres == canonical.FootprintFeatherMillimetres &&
               command.BlendInTicks == canonical.BlendInTicks && command.HoldTicks == canonical.HoldTicks;
    }
    return command.IsGlobal == false ||
           (command.FootprintWorld == format::WeatherBoundsWorld{} && command.FootprintFeatherMillimetres == 0);
}
} // namespace

WeatherSimulationStatus WeatherSimulation::TrySaveState(foundation::Array<uint8>& outBytes) const noexcept
{
    if (IsMounted() == false)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    const usize size = STATE_HEADER_BYTES + mState.ActiveOverrides.GetSize() * OVERRIDE_RECORD_BYTES +
                       mState.PendingCommands.GetSize() * COMMAND_RECORD_BYTES;
    foundation::Array<uint8> bytes;
    if (bytes.TryResize(size) == false)
    {
        return WeatherSimulationStatus::AllocationFailed;
    }
    const format::ClimateHeaderRecord& header = mClimate.GetHeader();
    const foundation::Fingerprint128 fingerprint = mClimate.GetFingerprint();
    format::WeatherByteWriter writer{ .Bytes = std::span<uint8>(bytes.GetData(), bytes.GetSize()) };
    for (const char byte : STATE_MAGIC)
    {
        writer.WriteUnsigned(static_cast<uint8>(byte));
    }
    writer.WriteUnsigned(STATE_VERSION);
    writer.WriteUnsigned(header.ClimateId);
    writer.WriteUnsigned(header.ClimateRevision);
    writer.WriteUnsigned(header.TicksPerSecond);
    writer.WriteUnsigned(fingerprint.Low);
    writer.WriteUnsigned(fingerprint.High);
    writer.WriteUnsigned(mSeed);
    writer.WriteUnsigned(mState.Tick);
    writer.WriteUnsigned(mState.NextEventOrdinal);
    writer.WriteUnsigned(static_cast<uint32>(mState.ActiveOverrides.GetSize()));
    writer.WriteUnsigned(static_cast<uint32>(mState.PendingCommands.GetSize()));
    for (const WeatherOverrideState& state : mState.ActiveOverrides)
    {
        WriteOverride(writer, state);
    }
    for (const WeatherOverrideCommand& command : mState.PendingCommands)
    {
        WriteCommand(writer, command);
    }
    LUDUS_REQUIRE(writer.HasFailed == false && writer.Offset == bytes.GetSize());
    outBytes = foundation::Move(bytes);
    return WeatherSimulationStatus::Success;
}

WeatherSimulationStatus WeatherSimulation::TryLoadState(foundation::BorrowedView<const uint8> bytes) noexcept
{
    if (IsMounted() == false || bytes.IsValid() == false)
    {
        return WeatherSimulationStatus::InvalidArgument;
    }
    format::WeatherByteReader reader{ .Bytes = std::span<const uint8>(bytes.GetDataOrNull(), bytes.GetSize()) };
    for (const char byte : STATE_MAGIC)
    {
        if (reader.ReadUnsigned<uint8>() != static_cast<uint8>(byte) || reader.HasFailed == true)
        {
            return WeatherSimulationStatus::InvalidState;
        }
    }
    if (reader.ReadUnsigned<uint32>() != STATE_VERSION || reader.HasFailed == true)
    {
        return reader.HasFailed == true ? WeatherSimulationStatus::InvalidState
                                        : WeatherSimulationStatus::UnsupportedStateVersion;
    }
    const uint32 climateId = reader.ReadUnsigned<uint32>();
    const uint32 climateRevision = reader.ReadUnsigned<uint32>();
    const uint32 ticksPerSecond = reader.ReadUnsigned<uint32>();
    foundation::Fingerprint128 fingerprint;
    fingerprint.Low = reader.ReadUnsigned<uint64>();
    fingerprint.High = reader.ReadUnsigned<uint64>();
    const uint64 seed = reader.ReadUnsigned<uint64>();
    WeatherSimulationState state;
    state.Tick = reader.ReadUnsigned<uint64>();
    state.NextEventOrdinal = reader.ReadUnsigned<uint64>();
    const uint32 activeCount = reader.ReadUnsigned<uint32>();
    const uint32 pendingCount = reader.ReadUnsigned<uint32>();
    if (reader.HasFailed == true)
    {
        return WeatherSimulationStatus::InvalidState;
    }
    const format::ClimateHeaderRecord& header = mClimate.GetHeader();
    if (climateId != header.ClimateId || climateRevision != header.ClimateRevision ||
        ticksPerSecond != header.TicksPerSecond || fingerprint != mClimate.GetFingerprint())
    {
        return WeatherSimulationStatus::IncompatibleClimate;
    }
    if (activeCount > MAX_ACTIVE_WEATHER_OVERRIDES || pendingCount > MAX_PENDING_WEATHER_COMMANDS ||
        reader.GetRemaining() != activeCount * OVERRIDE_RECORD_BYTES + pendingCount * COMMAND_RECORD_BYTES)
    {
        return WeatherSimulationStatus::InvalidState;
    }
    for (uint32 i = 0; i < activeCount; ++i)
    {
        WeatherOverrideState active;
        if (TryReadOverride(reader, active) == false || IsOverrideStateValid(mClimate, active, state.Tick) == false ||
            (state.ActiveOverrides.IsEmpty() == false &&
             state.ActiveOverrides.GetLast().OverrideId >= active.OverrideId))
        {
            return WeatherSimulationStatus::InvalidState;
        }
        LUDUS_REQUIRE(state.ActiveOverrides.TryAdd(active) == true);
    }
    for (uint32 i = 0; i < pendingCount; ++i)
    {
        WeatherOverrideCommand command;
        if (TryReadCommand(reader, command) == false ||
            internal::ValidateCommand(mClimate, command) != WeatherSimulationStatus::Success ||
            command.ApplyTick <= state.Tick || IsCommandCanonical(command) == false ||
            (state.PendingCommands.IsEmpty() == false &&
             internal::IsCommandBefore(state.PendingCommands.GetLast(), command) == false))
        {
            return WeatherSimulationStatus::InvalidState;
        }
        LUDUS_REQUIRE(state.PendingCommands.TryAdd(command) == true);
    }
    LUDUS_REQUIRE(reader.HasFailed == false && reader.GetRemaining() == 0);
    mSeed = seed;
    mState = foundation::Move(state);
    return WeatherSimulationStatus::Success;
}
} // namespace ludus::gameplay::weather
