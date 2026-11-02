#include <ludus/gameplay/sequencer/time.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::gameplay::sequencer
{
namespace
{
foundation::uint32 ComputeGreatestCommonDivisor(foundation::uint32 left, foundation::uint32 right) noexcept
{
    while (right != 0)
    {
        const foundation::uint32 remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}
} // namespace

SequenceStatus TryGetSequenceFrameTime(const SequenceFrameSchedule& schedule,
                                       foundation::uint64 frameOrdinal,
                                       SequenceTime& outTime) noexcept
{
    SequenceFrameRate rate = schedule.Rate;
    foundation::uint32 ticksPerSecond = schedule.TicksPerSecond;
    if (rate.Numerator == 0 || rate.Denominator == 0 || ticksPerSecond == 0)
    {
        return SequenceStatus::InvalidArgument;
    }
    foundation::uint32 divisor = ComputeGreatestCommonDivisor(rate.Numerator, rate.Denominator);
    rate.Numerator /= divisor;
    rate.Denominator /= divisor;
    divisor = ComputeGreatestCommonDivisor(rate.Numerator, ticksPerSecond);
    rate.Numerator /= divisor;
    ticksPerSecond /= divisor;
    const foundation::uint64 tickNumerator = foundation::uint64{ticksPerSecond} * rate.Denominator;
    const foundation::uint64 denominator = rate.Numerator;
    const foundation::uint64 wholeIncrement = tickNumerator / denominator;
    const foundation::uint64 remainder = tickNumerator % denominator;

    // Decompose the ordinal before multiplying: every remainder product fits
    // uint64 because both factors are below the uint32 denominator. Whole parts
    // are checked independently, without requiring a nonstandard 128-bit type.
    foundation::uint64 wholeTicks = 0;
    foundation::uint64 remainderTicks = 0;
    if (foundation::TryMultiply(frameOrdinal, wholeIncrement, wholeTicks) == false ||
        foundation::TryMultiply(frameOrdinal / denominator, remainder, remainderTicks) == false)
    {
        return SequenceStatus::OutOfRange;
    }
    const foundation::uint64 remainderProduct = (frameOrdinal % denominator) * remainder;
    if (foundation::TryAdd(wholeTicks, remainderTicks, wholeTicks) == false ||
        foundation::TryAdd(wholeTicks, remainderProduct / denominator, wholeTicks) == false ||
        foundation::TryAdd(wholeTicks, schedule.StartTick, wholeTicks) == false)
    {
        return SequenceStatus::OutOfRange;
    }
    const foundation::uint32 numerator = static_cast<foundation::uint32>(remainderProduct % denominator);
    divisor = ComputeGreatestCommonDivisor(numerator, rate.Numerator);
    outTime = {wholeTicks, numerator / divisor, rate.Numerator / divisor};
    return SequenceStatus::Success;
}
} // namespace ludus::gameplay::sequencer
