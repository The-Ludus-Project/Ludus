#include <ludus/foundation/profiling/trace_system.hpp>

#include "internal/memory_gauge.hpp"
#include "internal/recorder.hpp"
#include "internal/trace_chunk.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include <cstdio>
#include <string>

// -----------------------------------------------------------------------------
// Thanks to Google/Chromium contributors, "Trace Event Format", duration,
// instant and flow events, for the phase/track/timestamp interchange contract:
// The publisher maintains this living specification (consulted 2026-10-11):
// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview
// Thanks also to Perfetto contributors, "Visualizing external trace formats with
// Perfetto", "Chrome JSON format", pinned documentation confirming those fields
// and supported phases, with a reference to the original specification:
// https://github.com/google/perfetto/blob/59400b15c6ff8186ba0bba69f78c2cd454eea8b4/docs/getting-started/other-formats.md
// Adopt Chrome JSON
// B/E, instant and flow records; synthesized incomplete ends are Ludus policy.
// Design owner: docs/architecture/profiling-final.md.
//
// Perfetto / Chrome Trace Event JSON exporter (final design §16 primary path).
//
// This is the ANALYSIS + VISUALIZATION-export layer: it runs entirely off the
// hot path, after EndCapture(). It drains the recorder's chunks, orders each
// thread's events by timestamp, and emits the Chrome Trace Event format that
// ui.perfetto.dev and chrome://tracing consume directly. The hierarchical tree,
// inclusive/exclusive time, and calls-per-frame are RECONSTRUCTED by the viewer
// from the ordered B/E stream (C1: tree is a derived view, never stored).
//
// Mapping:
//   Begin  -> phase "B"   (viewer stacks these per thread -> the tree)
//   End    -> phase "E"
//   Instant-> phase "i"   (scope "t")
//   FrameMark -> phase "i" on a synthetic "Frames" name (a frame boundary; D7)
//   FlowOut/FlowIn -> phases "s"/"f" with a shared id (cross-context links; C5)
//   Counter -> phase "C" with args {"value": N} (timeline counter sample)
//   MemoryGauge -> one phase "C" event per domain sample, named "Memory/<domain>",
//              whose args are that sample's metrics; the coverage manifest goes
//              in the top-level "metadata" object (memory profiling phase 1)
//   registered thread names -> metadata "M" thread_name records
//
// Incomplete scopes (a Begin with no End when the capture ends, or a dropped
// chunk) are closed with a synthesized "E" carrying an "incomplete" arg so the
// duration is visibly untrustworthy rather than silently fabricated (§6, G13).
//
// Growable event/chunk buffers use Ludus::Array.
// std::string is still used for the JSON output buffer and the file path: it is
// slated for a Ludus String (ADR 0003) and stays until that exists; this is a
// cold, off-hot-path .cpp (never a public header). <cstdio> for the file write
// is within the STL policy on this cold path.
// -----------------------------------------------------------------------------

namespace ludus::foundation::profiling
{

namespace
{

using internal::TraceChunk;
using internal::TraceEvent;
using internal::TraceEventKind;
using internal::TraceRecorder;

struct FlatEvent
{
    uint64 Ticks;
    uint32 ThreadId;
    uint32 SiteId;
    uint16 Kind;
    uint16 Flags;
    uint64 Aux;
};

void AppendJsonEscaped(std::string& out, std::string_view text) noexcept
{
    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
}

void AppendUint(std::string& out, uint64 value) noexcept
{
    char buffer[24];
    const int n = std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    if (n > 0)
    {
        out.append(buffer, static_cast<usize>(n));
    }
}

// Chrome trace timestamps are microseconds (floating allowed). We keep integer
// ns internally and convert to a fixed-point microsecond string so sub-µs
// precision survives (ns / 1000 with three decimals).
void AppendMicros(std::string& out, uint64 ticksNs) noexcept
{
    const uint64 whole = ticksNs / 1000ull;
    const uint64 frac = ticksNs % 1000ull;
    char buffer[40];
    const int n = std::snprintf(buffer,
                                sizeof(buffer),
                                "%llu.%03llu",
                                static_cast<unsigned long long>(whole),
                                static_cast<unsigned long long>(frac));
    if (n > 0)
    {
        out.append(buffer, static_cast<usize>(n));
    }
}

// One Chrome trace-event object's data. Bundled into a struct (rather than a
// long list of same-typed scalars) so the numeric fields cannot be transposed
// at a call site.
struct ChromeEvent
{
    std::string_view Name;
    char Phase = 'i'; // "B"/"E"/"i"/"s"/"f"
    uint32 ThreadId = 0;
    uint64 TicksNs = 0;
    uint64 FlowId = 0;
    uint64 CounterValue = 0;
    bool Incomplete = false;
};

void AppendThreadName(std::string& inoutJson, bool& inoutIsFirst, uint32 threadId, std::string_view name) noexcept
{
    if (inoutIsFirst == false)
    {
        inoutJson += ",\n";
    }
    inoutIsFirst = false;
    inoutJson += "  {\"pid\":1,\"tid\":";
    AppendUint(inoutJson, threadId);
    inoutJson += ",\"ph\":\"M\",\"name\":\"thread_name\",\"args\":{\"name\":\"";
    AppendJsonEscaped(inoutJson, name);
    inoutJson += "\"}}";
}

void AppendEventObject(std::string& out, bool& first, const ChromeEvent& event) noexcept
{
    if (first == false)
    {
        out += ",\n";
    }
    first = false;
    out += "  {\"pid\":1,\"tid\":";
    AppendUint(out, event.ThreadId);
    out += ",\"ts\":";
    AppendMicros(out, event.TicksNs);
    out += ",\"ph\":\"";
    out += event.Phase;
    out += "\",\"name\":\"";
    AppendJsonEscaped(out, event.Name.empty() ? std::string_view{"?"} : event.Name);
    out += '"';
    if (event.Phase == 'i')
    {
        out += ",\"s\":\"t\"";
    }
    if (event.Phase == 's' || event.Phase == 'f')
    {
        out += ",\"cat\":\"flow\",\"id\":";
        AppendUint(out, event.FlowId);
        if (event.Phase == 'f')
        {
            out += ",\"bp\":\"e\"";
        }
    }
    if (event.Phase == 'C')
    {
        out += ",\"args\":{\"value\":";
        AppendUint(out, event.CounterValue);
        out += '}';
    }
    if (event.Incomplete)
    {
        out += ",\"args\":{\"incomplete\":true}";
    }
    out += '}';
}

// One domain sample being written as a single counter event. Its metric events
// are consecutive in a thread's ordered stream, share one timestamp and are
// emitted in increasing metric order, so a repeated or lower metric starts the
// next sample even when a coarse clock gives two samples the same tick.
struct GaugeGroup
{
    bool IsOpen = false;
    bool HasArgument = false;
    uint32 ThreadId = 0;
    uint32 DomainId = 0;
    uint32 LastMetric = 0;
    uint64 Ticks = 0;
};

using SeenDomains = foundation::StaticArray<bool, MAX_ALLOCATION_DOMAINS + 1>;

void CloseGaugeGroup(std::string& inoutJson, GaugeGroup& inoutGroup) noexcept
{
    if (inoutGroup.IsOpen == true)
    {
        inoutJson += "}}";
        inoutGroup.IsOpen = false;
    }
}

void AppendDomainName(std::string& inoutJson, uint32 domainId) noexcept
{
    RegisteredAllocationDomain entry;
    if (domainId != 0 && TryGetRegisteredAllocationDomain(domainId - 1, entry) == true && entry.NameOrNull != nullptr)
    {
        AppendJsonEscaped(inoutJson, entry.NameOrNull);
        return;
    }
    inoutJson += "domain-";
    AppendUint(inoutJson, domainId);
}

void AppendGaugeMetric(std::string& inoutJson,
                       bool& inoutIsFirst,
                       GaugeGroup& inoutGroup,
                       SeenDomains& inoutSeenDomains,
                       const FlatEvent& event) noexcept
{
    const uint32 domainId = internal::DecodeMemoryGaugeDomain(event.SiteId);
    const uint32 metric = internal::DecodeMemoryGaugeMetric(event.SiteId);
    const bool isSameSample = inoutGroup.IsOpen == true && inoutGroup.ThreadId == event.ThreadId &&
                              inoutGroup.Ticks == event.Ticks && inoutGroup.DomainId == domainId &&
                              metric > inoutGroup.LastMetric;
    if (isSameSample == false)
    {
        CloseGaugeGroup(inoutJson, inoutGroup);
        if (inoutIsFirst == false)
        {
            inoutJson += ",\n";
        }
        inoutIsFirst = false;
        inoutJson += "  {\"pid\":1,\"tid\":";
        AppendUint(inoutJson, event.ThreadId);
        inoutJson += ",\"ts\":";
        AppendMicros(inoutJson, event.Ticks);
        inoutJson += ",\"ph\":\"C\",\"name\":\"Memory/";
        AppendDomainName(inoutJson, domainId);
        inoutJson += "\",\"args\":{";
        inoutGroup.IsOpen = true;
        inoutGroup.HasArgument = false;
        inoutGroup.ThreadId = event.ThreadId;
        inoutGroup.DomainId = domainId;
        inoutGroup.Ticks = event.Ticks;
        if (domainId < inoutSeenDomains.GetSize())
        {
            inoutSeenDomains[domainId] = true;
        }
    }
    if (inoutGroup.HasArgument == true)
    {
        inoutJson += ',';
    }
    inoutGroup.HasArgument = true;
    inoutGroup.LastMetric = metric;
    inoutJson += '"';
    inoutJson += internal::GetMemoryGaugeMetricName(metric);
    inoutJson += "\":";
    AppendUint(inoutJson, event.Aux);
}

// Coverage manifest for exported gauges: what the samples include, what they
// cannot include, and how to read them. Registry names are restricted to a
// JSON-safe character set; they are escaped anyway.
void AppendMemoryGaugeMetadata(std::string& inoutJson, const SeenDomains& seenDomains) noexcept
{
    inoutJson += ",\"metadata\":{\"ludus-memory-gauges\":{\"schema\":";
    AppendUint(inoutJson, internal::MEMORY_GAUGE_SCHEMA_VERSION);
    inoutJson += ",\"metric\":\"requested-bytes\"";
    inoutJson += ",\"sampling\":\"relaxed per-field samples; exact only at a caller-established quiescent boundary\"";
    inoutJson += ",\"peaks\":\"per-domain high-water marks; never sum them into a process peak\"";
    inoutJson += ",\"validity\":{\"0\":\"valid\",\"1\":\"saturated\",\"2\":\"accounting-error\"}";
    inoutJson += ",\"coverage\":\"registered allocation domains only; no measured whole-process denominator\"";
    inoutJson += ",\"includedDomains\":[";
    bool isFirstDomain = true;
    for (usize domainId = 1; domainId < seenDomains.GetSize(); ++domainId)
    {
        if (seenDomains[domainId] == false)
        {
            continue;
        }
        if (isFirstDomain == false)
        {
            inoutJson += ',';
        }
        isFirstDomain = false;
        inoutJson += "{\"id\":";
        AppendUint(inoutJson, domainId);
        inoutJson += ",\"name\":\"";
        AppendDomainName(inoutJson, static_cast<uint32>(domainId));
        inoutJson += "\"}";
    }
    inoutJson += "],\"excludedPaths\":[";
    for (usize index = 0; index < GetAllocationCoverageExclusionCount(); ++index)
    {
        const AllocationCoverageExclusion& exclusion = GetAllocationCoverageExclusion(index);
        if (index != 0)
        {
            inoutJson += ',';
        }
        inoutJson += "{\"name\":\"";
        AppendJsonEscaped(inoutJson, exclusion.Name);
        inoutJson += "\",\"reason\":\"";
        AppendJsonEscaped(inoutJson, exclusion.Reason);
        inoutJson += "\"}";
    }
    inoutJson += "]}}";
}

} // namespace

bool ExportPerfettoTrace(std::string_view path) noexcept
{
    TraceRecorder& recorder = TraceRecorder::Instance();

    // Drain every full chunk. Flatten to a single Vector we can stable-sort by
    // (threadId, ticks) so each thread's B/E stream is monotonic — the viewer
    // relies on ordering to nest slices. FlatEvent is a POD, so Vector growth
    // relocates it with a single memcpy.
    //
    // The full-chunk stack is LIFO, and each producer publishes its own chunks
    // in recording order, so flattening the drained list back to front restores
    // every thread's recording order. The stable sort then keeps equal-tick
    // events (an End and the next Begin on a fast clock) in recorded order
    // across a chunk boundary instead of emitting an End before its Begin.
    foundation::Array<TraceChunk*> drained;
    for (TraceChunk* chunk = recorder.DrainFullChunks(); chunk != nullptr; chunk = chunk->PoolNextOrNull)
    {
        drained.Add(chunk);
    }
    foundation::Array<FlatEvent> events;
    for (usize chunkIndex = drained.GetSize(); chunkIndex > 0; --chunkIndex)
    {
        const TraceChunk& chunk = *drained[chunkIndex - 1];
        for (uint32 i = 0; i < chunk.Count; ++i)
        {
            const TraceEvent& e = chunk.Events[i];
            events.Add(FlatEvent{e.Ticks, chunk.ThreadId, e.SiteId, e.Kind, e.Flags, e.Aux});
        }
    }

    // Recycle chunks back to the pool now that we've copied their events.
    for (TraceChunk* c : drained)
    {
        recorder.RecycleChunk(c);
    }

    if (events.IsEmpty())
    {
        return false;
    }

    foundation::Array<FlatEvent> scratch;
    if (scratch.TryResize(events.GetSize()) == false)
    {
        return false;
    }
    if (foundation::StableSort(events.GetData(),
                               scratch.GetData(),
                               events.GetSize(),
                               [](const FlatEvent& a, const FlatEvent& b) noexcept {
                                   if (a.ThreadId != b.ThreadId)
                                   {
                                       return a.ThreadId < b.ThreadId;
                                   }
                                   return a.Ticks < b.Ticks;
                               }) == false)
    {
        return false;
    }

    std::string out;
    out.reserve(events.size() * 96 + 256);
    out += "{\"traceEvents\":[\n";
    bool first = true;
    for (usize threadIndex = 0; threadIndex < recorder.ThreadCount(); ++threadIndex)
    {
        const uint32 threadId = recorder.ThreadIdAt(threadIndex);
        const std::string_view threadName = recorder.ThreadName(threadId);
        if (threadName.empty() == false)
        {
            AppendThreadName(out, first, threadId, threadName);
        }
    }

    // Per-thread open-scope DEPTH used to synthesize End events for scopes still
    // open at capture end (incomplete; §6/G13). The viewer pairs B/E by order, so
    // only the *count* of unclosed Begins per thread matters here — the previous
    // pair of parallel per-thread stacks (ticks and names) pushed values that
    // were never read. A single depth counter is equivalent and clearer (and
    // removes the std::string dependency entirely).
    usize openDepth = 0;
    uint32 currentThread = events.GetFirst().ThreadId;
    GaugeGroup gaugeGroup;
    SeenDomains seenDomains{};
    bool hasMemoryGauges = false;

    uint64 lastTicks = events.GetFirst().Ticks;

    auto flushOpen = [&](uint32 threadId) noexcept {
        // Close any scopes left open on this thread with a synthesized,
        // incomplete End at the last-seen timestamp (§6/G13: labelled, never a
        // silently fabricated duration).
        while (openDepth > 0)
        {
            ChromeEvent synth;
            synth.Phase = 'E';
            synth.ThreadId = threadId;
            synth.TicksNs = lastTicks;
            synth.Incomplete = true;
            AppendEventObject(out, first, synth);
            --openDepth;
        }
    };

    for (const FlatEvent& e : events)
    {
        if (static_cast<TraceEventKind>(e.Kind) != TraceEventKind::MemoryGauge || e.ThreadId != currentThread)
        {
            CloseGaugeGroup(out, gaugeGroup);
        }
        if (e.ThreadId != currentThread)
        {
            flushOpen(currentThread);
            currentThread = e.ThreadId;
        }
        lastTicks = e.Ticks;
        const std::string_view name = recorder.SiteName(e.SiteId);

        ChromeEvent ce;
        ce.Name = name;
        ce.ThreadId = e.ThreadId;
        ce.TicksNs = e.Ticks;

        switch (static_cast<TraceEventKind>(e.Kind))
        {
            case TraceEventKind::Begin:
                ce.Phase = 'B';
                AppendEventObject(out, first, ce);
                ++openDepth;
                break;
            case TraceEventKind::End:
                ce.Phase = 'E';
                AppendEventObject(out, first, ce);
                if (openDepth > 0)
                {
                    --openDepth;
                }
                break;
            case TraceEventKind::Instant:
                ce.Phase = 'i';
                AppendEventObject(out, first, ce);
                break;
            case TraceEventKind::FrameMark:
                ce.Name = std::string_view{"FrameMark"};
                ce.Phase = 'i';
                AppendEventObject(out, first, ce);
                break;
            case TraceEventKind::FlowOut:
                // Emit the flow start.
                ce.Phase = 's';
                ce.FlowId = e.Aux;
                AppendEventObject(out, first, ce);
                break;
            case TraceEventKind::FlowIn:
                ce.Phase = 'f';
                ce.FlowId = e.Aux;
                AppendEventObject(out, first, ce);
                break;
            case TraceEventKind::Counter:
                ce.Phase = 'C';
                ce.CounterValue = e.Aux;
                AppendEventObject(out, first, ce);
                break;
            case TraceEventKind::MemoryGauge:
                AppendGaugeMetric(out, first, gaugeGroup, seenDomains, e);
                hasMemoryGauges = true;
                break;
        }
    }
    CloseGaugeGroup(out, gaugeGroup);
    flushOpen(currentThread);
    (void)lastTicks;

    out += "\n]";
    if (hasMemoryGauges == true)
    {
        AppendMemoryGaugeMetadata(out, seenDomains);
    }
    out += ",\"displayTimeUnit\":\"ns\"}\n";

    std::FILE* file = std::fopen(std::string(path).c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    const usize written = std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    return written == out.size();
}

} // namespace ludus::foundation::profiling
