#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/gameplay/weather/climate.h>
#include <ludus/gameplay/weather/weather_simulation.h>

// Pure fixed-point resolution rules shared by the simulation and its state codec. Every function is
// integer-only, so native and browser targets compute identical codes.
namespace ludus::gameplay::weather::internal
{
/// Divide with rounding half away from zero. denominator must be positive.
[[nodiscard]] int64 DivideRounded(int64 numerator, int64 denominator) noexcept;

/// Linear interpolation from start toward end by numerator / denominator, rounded half away from zero.
[[nodiscard]] int32 InterpolateCode(int32 start, int32 end, uint64 numerator, uint64 denominator) noexcept;

/// Interpolate every channel.
[[nodiscard]] weather_format::WeatherChannelCodes InterpolateChannels(const weather_format::WeatherChannelCodes& start,
                                                                      const weather_format::WeatherChannelCodes& end,
                                                                      uint64 numerator,
                                                                      uint64 denominator) noexcept;

/// Product of two fraction codes as a fraction code, rounded half up. Both inputs lie in [0, FRACTION_ONE].
[[nodiscard]] int32 MultiplyFractions(int32 left, int32 right) noexcept;

/// Feathered coverage of a position by bounds, as a fraction code. Zero on and outside the edge, rising
/// with a smoothstep to FRACTION_ONE at the feather depth inside it; a zero feather gives a hard edge.
[[nodiscard]] int32 ComputeFeatheredCoverage(const weather_format::WeatherBoundsWorld& bounds,
                                             uint32 featherMillimetres,
                                             int64 positionXWorld,
                                             int64 positionZWorld) noexcept;

/// Location of a tick within a looping schedule.
struct SchedulePosition final
{
    /// Entry index relative to the schedule's first entry.
    uint32 Entry = 0;
    /// Ticks since the entry started.
    uint64 Offset = 0;
};

/// Locate a tick in a schedule: entries play once from tick zero, then the range from LoopStartEntry
/// repeats. The schedule index must be valid for the climate.
[[nodiscard]] SchedulePosition LocateSchedule(const Climate& climate, uint32 scheduleIndex, uint64 tick) noexcept;

/// Channel values of a schedule at a tick: the held preset, or the linear blend toward the next entry.
[[nodiscard]] weather_format::WeatherChannelCodes
EvaluateSchedule(const Climate& climate, uint32 scheduleIndex, uint64 tick) noexcept;

/// Weight fraction code of an active override at a tick at or after its phase start.
[[nodiscard]] int32 EvaluateOverrideWeight(const WeatherOverrideState& state, uint64 tick) noexcept;

/// Channel values of an active override at a tick at or after its phase start, before position weighting.
[[nodiscard]] weather_format::WeatherChannelCodes EvaluateOverrideSource(const WeatherOverrideState& state,
                                                                         uint64 tick) noexcept;

/// Whether an override command precedes another in application order.
[[nodiscard]] bool IsCommandBefore(const WeatherOverrideCommand& left, const WeatherOverrideCommand& right) noexcept;

/// Whether an active override precedes another in resolution order: descending priority, then ascending ID.
[[nodiscard]] bool IsOverrideBefore(const WeatherOverrideState& left, const WeatherOverrideState& right) noexcept;

/// Validate a command's fields against a climate, independently of the simulation state.
[[nodiscard]] WeatherSimulationStatus ValidateCommand(const Climate& climate,
                                                      const WeatherOverrideCommand& command) noexcept;
} // namespace ludus::gameplay::weather::internal
