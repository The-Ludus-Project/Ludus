#pragma once
#include "lifecycle.h"
#include "raster.h"
#include <ludus/graphics/rhi/measurement.h>
namespace ludus::graphics::rhi::internal
{
/// Widest timestamp counter, in bits, and the coarsest accepted tick, in nanoseconds (one second).
inline constexpr foundation::uint32 MAX_TIMESTAMP_COUNTER_BITS = 64;
inline constexpr foundation::float64 MAX_TIMESTAMP_TICK_NANOSECONDS = 1e9;
// A modular timestamp difference is trustworthy only inside one counter period.
struct TimestampCounter final
{
    foundation::uint32 Bits;
    foundation::float64 Tick;
};
/// Mask of a counter's valid low bits; bits must lie in [1, MAX_TIMESTAMP_COUNTER_BITS].
inline foundation::uint64 GetTimestampMask(foundation::uint32 bits) noexcept
{
    return bits == MAX_TIMESTAMP_COUNTER_BITS ? ~foundation::uint64{0} : (foundation::uint64{1} << bits) - 1;
}
inline bool TimestampDuration(foundation::uint64 start,
                              foundation::uint64 end,
                              TimestampCounter counter,
                              foundation::uint64 elapsedCpu,
                              foundation::float64& output) noexcept
{
    if (counter.Bits == 0 || counter.Bits > MAX_TIMESTAMP_COUNTER_BITS || (counter.Tick > 0) == false ||
        (counter.Tick < MAX_TIMESTAMP_TICK_NANOSECONDS) == false)
    {
        return false;
    }
    foundation::float64 period = counter.Tick;
    for (foundation::uint32 bit = 0; bit < counter.Bits; ++bit)
    {
        period *= 2;
    }
    if (static_cast<foundation::float64>(elapsedCpu) >= period)
    {
        return false;
    }
    const foundation::float64 duration =
        static_cast<foundation::float64>((end - start) & GetTimestampMask(counter.Bits)) * counter.Tick;
    if (duration > static_cast<foundation::float64>(elapsedCpu) + counter.Tick * 2)
    {
        return false;
    }
    output = duration;
    return true;
}
/// Largest accepted bound, in nanoseconds, on a device-to-NowTicks calibration's error. A larger
/// bound leaves the pass batch duration-only rather than misplacing execution slices.
inline constexpr foundation::uint64 MAX_CLOCK_UNCERTAINTY_NANOSECONDS = 50'000;
/// One device timestamp and the foundation::time::NowTicks nanosecond it was paired with.
struct ClockCalibration final
{
    foundation::uint64 Ticks = 0;
    foundation::uint64 DeviceTimestamp = 0;
};
/// Start and end of one operation, in device timestamps or NowTicks nanoseconds.
struct TimestampInterval final
{
    foundation::uint64 Start = 0;
    foundation::uint64 End = 0;
};
// Linear interpolation between two calibrations that bracket the measured work. Device
// timestamps are compared modulo validBits, so the caller must already have established
// that the calibration window spans less than one counter period. An interval outside the
// window is rejected. Results truncate; the sub-nanosecond error is far below any accepted
// calibration uncertainty. Failure preserves outTicks.
inline bool MapCalibratedInterval(const ClockCalibration& first,
                                  const ClockCalibration& last,
                                  foundation::uint32 validBits,
                                  TimestampInterval device,
                                  TimestampInterval& outTicks) noexcept
{
    if (validBits == 0 || validBits > MAX_TIMESTAMP_COUNTER_BITS || last.Ticks <= first.Ticks)
    {
        return false;
    }
    const foundation::uint64 mask = GetTimestampMask(validBits);
    const foundation::uint64 window = (last.DeviceTimestamp - first.DeviceTimestamp) & mask;
    const foundation::uint64 startOffset = (device.Start - first.DeviceTimestamp) & mask;
    const foundation::uint64 endOffset = (device.End - first.DeviceTimestamp) & mask;
    if (window == 0 || startOffset > endOffset || endOffset > window)
    {
        return false;
    }
    const foundation::float64 rate =
        static_cast<foundation::float64>(last.Ticks - first.Ticks) / static_cast<foundation::float64>(window);
    outTicks =
    {
        .Start = first.Ticks + static_cast<foundation::uint64>(static_cast<foundation::float64>(startOffset) * rate),
        .End = first.Ticks + static_cast<foundation::uint64>(static_cast<foundation::float64>(endOffset) * rate),
    };
    return true;
}
template <foundation::usize N>
void CopyMeasurementName(const char* source, foundation::usize length, char (&destination)[N]) noexcept
{
    foundation::usize index = 0;
    while (source != nullptr && index + 1 < N && index < length && source[index] != '\0')
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}
// A failed allocation attempt must preserve the preceding observable terminal sample.
class TimingArmRollback final
{
public:
    explicit TimingArmRollback(GpuTiming& output) noexcept : Output(output), Previous(output), Owner(DeviceOwner()) {}
    ~TimingArmRollback() noexcept
    {
        if (Accepted == false && DeviceOwner() == Owner && GetStartup().State == StartupState::Ready)
        {
            Output = Previous;
        }
    }
    TimingArmRollback(const TimingArmRollback&) = delete;
    TimingArmRollback& operator=(const TimingArmRollback&) = delete;
    void Accept() noexcept
    {
        Accepted = true;
    }

private:
    GpuTiming& Output;
    GpuTiming Previous;
    foundation::uint64 Owner;
    bool Accepted = false;
};
void SetGpuTimingRequest(bool) noexcept;
bool GpuTimingRequested() noexcept;
} // namespace ludus::graphics::rhi::internal
namespace ludus::graphics::rhi::backend
{
bool AllocatorBlockBytes(foundation::uint64&) noexcept;
MeasurementAdapter TimingAdapter() noexcept;
RasterStatus TimingArm() noexcept;
GpuTiming TimingPoll() noexcept;
void TimingReset() noexcept;
bool RasterAllocationBytes(internal::RasterKind, foundation::usize, foundation::uint64&) noexcept;
} // namespace ludus::graphics::rhi::backend
