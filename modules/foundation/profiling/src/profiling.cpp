#include <ludus/foundation/profiling/integration.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include "internal/recorder.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/profiling/clock.hpp>

#include <atomic>

// -----------------------------------------------------------------------------
// Type-erased instrumentation boundary + public control API (final design §9,
// §18, §22). All the hot-path detail::Emit* functions are compiled ONCE here so
// the ubiquitous profiling.hpp stays template- and <chrono>-free.
// -----------------------------------------------------------------------------

namespace ludus::foundation::profiling
{

namespace
{

// Monotonic flow-id source. Flow correlation logic is deferred (Phase 4), but
// the id must be unique now so a FLOW_OUT/FLOW_IN pair matches.
std::atomic<uint64> gNextFlowId{1};

// Build a base event with the current timestamp.
[[nodiscard]] internal::TraceEvent MakeEvent(internal::TraceEventKind kind, uint32 siteId) noexcept
{
    internal::TraceEvent event;
    event.Ticks = NowTicks();
    event.SiteId = siteId;
    event.Kind = static_cast<uint16>(kind);
    event.Flags = internal::TRACE_FLAG_NONE;
    event.Aux = 0;
    return event;
}

} // namespace

namespace detail
{

bool CaptureActive() noexcept
{
    return internal::IsCaptureActive();
}

void EmitBegin(const ZoneDescriptor& descriptor) noexcept
{
    // One relaxed load + predicted-not-taken branch when idle (§19, gate G2).
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::Begin, descriptor.SiteId);
    if (descriptor.CategoryId != 0)
    {
        event.Flags |= internal::TRACE_FLAG_HAS_CATEGORY;
        event.Aux = descriptor.CategoryId;
    }
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void EmitEnd(uint32 siteId) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    const internal::TraceEvent event = MakeEvent(internal::TraceEventKind::End, siteId);
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void EmitInstant(const ZoneDescriptor& descriptor) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    const internal::TraceEvent event = MakeEvent(internal::TraceEventKind::Instant, descriptor.SiteId);
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void EmitFrameMark() noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    const internal::TraceEvent event = MakeEvent(internal::TraceEventKind::FrameMark, 0);
    (void)internal::TraceRecorder::Instance().Emit(event);
}

uint64 EmitFlowOut(const ZoneDescriptor& descriptor) noexcept
{
    const uint64 flowId = gNextFlowId.fetch_add(1, std::memory_order_relaxed);
    if (internal::IsCaptureActive() == false)
    {
        return flowId;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::FlowOut, descriptor.SiteId);
    event.Flags |= internal::TRACE_FLAG_HAS_FLOW;
    event.Aux = flowId;
    (void)internal::TraceRecorder::Instance().Emit(event);
    return flowId;
}

void EmitFlowIn(uint64 flowId) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::FlowIn, 0);
    event.Flags |= internal::TRACE_FLAG_HAS_FLOW;
    event.Aux = flowId;
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void RegisterSite(const ZoneDescriptor& descriptor) noexcept
{
    internal::TraceRecorder::Instance().RegisterSite(descriptor.SiteId,
                                                     descriptor.Name,
                                                     descriptor.File,
                                                     descriptor.Line);
}

} // namespace detail

// ---- Integration emission API (integration.hpp) ----------------------------

bool IsCaptureActive() noexcept
{
    return internal::IsCaptureActive();
}

void RegisterSite(const ZoneDescriptor& descriptor) noexcept
{
    detail::RegisterSite(descriptor);
}

void BeginSite(uint32 siteId) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    const internal::TraceEvent event = MakeEvent(internal::TraceEventKind::Begin, siteId);
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void EndSite(uint32 siteId) noexcept
{
    detail::EmitEnd(siteId);
}

void StartFlow(const ZoneDescriptor& descriptor, uint64 flowId) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::FlowOut, descriptor.SiteId);
    event.Flags |= internal::TRACE_FLAG_HAS_FLOW;
    event.Aux = flowId;
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void FinishFlow(const ZoneDescriptor& descriptor, uint64 flowId) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::FlowIn, descriptor.SiteId);
    event.Flags |= internal::TRACE_FLAG_HAS_FLOW;
    event.Aux = flowId;
    (void)internal::TraceRecorder::Instance().Emit(event);
}

void SampleCounter(const ZoneDescriptor& descriptor, uint64 value) noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return;
    }
    internal::TraceEvent event = MakeEvent(internal::TraceEventKind::Counter, descriptor.SiteId);
    event.Aux = value;
    (void)internal::TraceRecorder::Instance().Emit(event);
}

// ---- Public control API (trace_system.hpp) ---------------------------------

void RegisterThreadForTrace(std::string_view threadName) noexcept
{
    (void)internal::TraceRecorder::Instance().RegisterThread(threadName);
}

void UnregisterThreadForTrace() noexcept
{
    internal::TraceRecorder::Instance().UnregisterThread();
}

bool BeginCapture(usize maxChunks) noexcept
{
    // Ensure the calling thread is registered so its events are attributed.
    if (internal::CurrentThreadRecorder().IsRegistered == false)
    {
        (void)internal::TraceRecorder::Instance().RegisterThread(std::string_view{"Main"});
    }
    return internal::TraceRecorder::Instance().BeginCapture(maxChunks);
}

void EndCapture() noexcept
{
    // Waits for in-flight producers and publishes every thread's partial chunk,
    // so the capture is complete before export.
    internal::TraceRecorder::Instance().EndCapture();
}

TraceHealth GetTraceHealth() noexcept
{
    const internal::TraceHealth health = internal::TraceRecorder::Instance().Health();
    TraceHealth out;
    out.EventsRecorded = health.EventsRecorded;
    out.EventsDropped = health.EventsDropped;
    out.ThreadsRegistered = health.ThreadsRegistered;
    return out;
}

} // namespace ludus::foundation::profiling
