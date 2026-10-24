#include <ludus/foundation/base/config.h>
#include <ludus/graphics/rhi/gpu_scope.h>

#include "internal/gpu_scope_backend.h"
#include "internal/measurement.h"
#include "internal/resources.h"

#include <ludus/foundation/strings/utf8.hpp>

#include <cstring>
#include <string_view>

namespace ludus::graphics::rhi
{
using namespace foundation;
bool MakeGpuScope(uint64 identity,
                  BorrowedView<const char> name,
                  GpuScopeDescription& outScope,
                  uint32 logicalPassCount) noexcept
{
    if (identity == 0 || logicalPassCount == 0 || name.IsValid() == false || name.IsEmpty() == true ||
        name.GetSize() >= GPU_SCOPE_NAME_BYTES)
    {
        return false;
    }
    // Adapt to the existing FoundationStrings validation boundary; no owning STL text.
    const std::string_view text(name.GetDataOrNull(), name.GetSize());
    Utf8View validated;
    usize errorOffset = 0;
    if (ValidateUtf8(text, validated, errorOffset) != StringStatus::Ok || ValidateCString(text) != StringStatus::Ok)
    {
        return false;
    }
    GpuScopeDescription candidate;
    candidate.Identity = identity;
    candidate.LogicalPassCount = logicalPassCount;
    std::memcpy(candidate.Name.GetData(), name.GetDataOrNull(), name.GetSize());
    outScope = candidate;
    return true;
}
RasterStatus ArmGpuPassTimings(DeviceHandle device) noexcept
{
    const RasterStatus status = internal::ValidateMeasurementDevice(device);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (internal::FrameOpen() == true)
    {
        return RasterStatus::InvalidState;
    }
    const RasterStatus armed =
        internal::GpuTimingRequested() == true ? backend::ArmPassTimings() : RasterStatus::Unsupported;
    const RasterStatus completed = internal::ValidateMeasurementDevice(device);
    return completed == RasterStatus::Ready ? armed : completed;
}
RasterStatus GetGpuPassTimings(DeviceHandle device, GpuPassTimings& outTimings) noexcept
{
    const RasterStatus status = internal::ValidateMeasurementDevice(device);
    if (status == RasterStatus::InvalidHandle)
    {
        return status;
    }
    outTimings = {};
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    outTimings = internal::GpuTimingRequested() == true ? backend::PollPassTimings()
                                                        : GpuPassTimings{ .State = GpuTimingState::Unavailable };
    const RasterStatus completed = internal::ValidateMeasurementDevice(device);
    if (completed != RasterStatus::Ready)
    {
        outTimings = {};
    }
    return completed;
}
RasterStatus BeginGpuScope(DeviceHandle device, const GpuScopeDescription& scope) noexcept
{
    const RasterStatus status = internal::ValidateMeasurementDevice(device);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (internal::FrameOpen() == false)
    {
        return RasterStatus::InvalidState;
    }
    usize length = 0;
    while (length < GPU_SCOPE_NAME_BYTES && scope.Name[length] != '\0')
    {
        ++length;
    }
    GpuScopeDescription validated;
    if (MakeGpuScope(scope.Identity, {scope.Name.GetData(), length}, validated, scope.LogicalPassCount) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    return backend::BeginScope(validated);
}
RasterStatus EndGpuScope(DeviceHandle device) noexcept
{
    const RasterStatus status = internal::ValidateMeasurementDevice(device);
    return status == RasterStatus::Ready ? backend::EndScope() : status;
}
GpuScope::GpuScope(DeviceHandle device, const GpuScopeDescription& scope) noexcept : mDevice(device)
{
    mIsActive = BeginGpuScope(device, scope) == RasterStatus::Ready;
}
GpuScope::~GpuScope() noexcept
{
    if (mIsActive == true)
    {
        static_cast<void>(EndGpuScope(mDevice));
    }
}
} // namespace ludus::graphics::rhi

// Browser backends and the Direct3D 12 bring-up record no GPU pass timings.
#if LUDUS_TARGET_OS == LUDUS_OS_WEB || LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
namespace ludus::graphics::rhi::backend
{
RasterStatus ArmPassTimings() noexcept
{
    return RasterStatus::Unsupported;
}
GpuPassTimings PollPassTimings() noexcept
{
    return { .State = GpuTimingState::Unavailable };
}
RasterStatus BeginScope(const GpuScopeDescription&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus EndScope() noexcept
{
    return RasterStatus::Unsupported;
}
void ResetPassTimings() noexcept {}
} // namespace ludus::graphics::rhi::backend
#endif
