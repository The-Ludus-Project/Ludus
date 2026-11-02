#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::gameplay::sequencer
{
/// Result of sequence preparation or sampling. Failure preserves the requested output.
enum class SequenceStatus : foundation::uint8
{
    Success,          ///< A complete validated result was published.
    InvalidArgument,  ///< An invalid view, time, rate, base value or enum was supplied.
    InvalidKeys,      ///< Keys are unordered, nonfinite, nonunit or outside the channel's domain.
    DuplicateBinding, ///< More than one track group names the same binding; combine its channels.
    BudgetExceeded,   ///< The requested track/key/storage payload exceeds the supplied limits.
    OutOfMemory,      ///< A fallible preparation allocation failed.
    OutOfRange,       ///< Checked time arithmetic or a sampled value is not representable.
    Unprepared,       ///< The sequence has no accepted prepared revision.
    MissingBinding,   ///< No track of the requested kind has that binding identity.
};

/// Positive rational frames per second; neither component may be zero.
struct SequenceFrameRate final
{
    foundation::uint32 Numerator = 1;   ///< Frames in Denominator seconds.
    foundation::uint32 Denominator = 1; ///< Seconds per Numerator frames.
};

/// Immutable clock inputs for an image sequence. Changing these creates a different sample schedule.
struct SequenceFrameSchedule final
{
    SequenceFrameRate Rate{};              ///< Positive rational output frames per second.
    foundation::uint32 TicksPerSecond = 1; ///< Positive authored tick frequency.
    foundation::uint64 StartTick = 0;      ///< Nonnegative start tick, added once to each absolute ordinal's time.
};

/// Nonnegative exact time in authored ticks: Tick + Numerator / Denominator.
/// Denominator must be positive and Numerator < Denominator. Equivalent proper
/// fractions are accepted; there is no floating-point clock or tick-grid rounding.
struct SequenceTime final
{
    foundation::uint64 Tick = 0;        ///< Whole ticks from the sequence epoch.
    foundation::uint32 Numerator = 0;   ///< Fractional tick numerator.
    foundation::uint32 Denominator = 1; ///< Fractional tick denominator.
};

/// Compute startTick + frameOrdinal * ticksPerSecond / rate exactly.
/// @param schedule Positive rate/frequency and nonnegative epoch; rate need not already be reduced.
/// @param frameOrdinal Absolute nonnegative output frame ordinal, including subset renders.
/// @param outTime Replaced by a reduced proper fraction on Success; unchanged otherwise.
/// @return InvalidArgument for a zero rate/frequency; OutOfRange when the resulting
/// whole tick exceeds uint64. Avoidable intermediate multiplication overflow is handled.
/// @note Allocation-free, no thread affinity. Negative/pre-roll times are not supported yet.
[[nodiscard]] SequenceStatus TryGetSequenceFrameTime(const SequenceFrameSchedule& schedule,
                                                     foundation::uint64 frameOrdinal,
                                                     SequenceTime& outTime) noexcept;
} // namespace ludus::gameplay::sequencer
