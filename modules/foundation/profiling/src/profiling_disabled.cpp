#include <ludus/foundation/profiling/integration.hpp>
#include <ludus/foundation/profiling/memory_gauges.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

namespace ludus::foundation::profiling
{
namespace detail
{
bool CaptureActive() noexcept
{
    return false;
}
void EmitBegin(const ZoneDescriptor&) noexcept {}
void EmitEnd(uint32) noexcept {}
void EmitInstant(const ZoneDescriptor&) noexcept {}
void EmitFrameMark() noexcept {}
uint64 EmitFlowOut(const ZoneDescriptor&) noexcept
{
    return 0;
}
void EmitFlowIn(uint64) noexcept {}
void RegisterSite(const ZoneDescriptor&) noexcept {}
} // namespace detail
bool IsCaptureActive() noexcept
{
    return false;
}
void RegisterSite(const ZoneDescriptor&) noexcept {}
void BeginSite(uint32) noexcept {}
void EndSite(uint32) noexcept {}
void StartFlow(const ZoneDescriptor&, uint64) noexcept {}
void FinishFlow(const ZoneDescriptor&, uint64) noexcept {}
void SampleCounter(const ZoneDescriptor&, uint64) noexcept {}
void RegisterThreadForTrace(std::string_view) noexcept {}
void UnregisterThreadForTrace() noexcept {}
bool BeginCapture(usize) noexcept
{
    return false;
}
void EndCapture() noexcept {}
bool ExportPerfettoTrace(std::string_view) noexcept
{
    return false;
}
TraceHealth GetTraceHealth() noexcept
{
    return {};
}
bool RecordMemoryGauges() noexcept
{
    return false;
}
} // namespace ludus::foundation::profiling
