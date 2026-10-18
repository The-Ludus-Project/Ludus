#pragma once
#include "lifecycle.h"
#include "raster.h"
#include <ludus/graphics/rhi/measurement.h>
namespace ludus::graphics::rhi::internal
{
// A modular timestamp difference is trustworthy only inside one counter period.
struct TimestampCounter final
{
    foundation::uint32 Bits;
    foundation::float64 Tick;
};
inline bool TimestampDuration(foundation::uint64 start,
                              foundation::uint64 end,
                              TimestampCounter counter,
                              foundation::uint64 elapsedCpu,
                              foundation::float64& output) noexcept
{
    if (counter.Bits == 0 || counter.Bits > 64 || (counter.Tick > 0) == false || (counter.Tick < 1e9) == false)
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
    const auto mask = counter.Bits == 64 ? ~foundation::uint64{0} : (foundation::uint64{1} << counter.Bits) - 1;
    const auto duration = static_cast<foundation::float64>((end - start) & mask) * counter.Tick;
    if (duration > static_cast<foundation::float64>(elapsedCpu) + counter.Tick * 2)
    {
        return false;
    }
    output = duration;
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
