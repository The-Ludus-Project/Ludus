#include "internal/viewport_trace.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/profiling/profiling.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 VIEWPORT_PROCESS_ID = 3;
// Keep tool-stage tracks apart from the per-viewport lane numbers and within
// the signed 32-bit thread IDs accepted by trace viewers.
constexpr uint64 SHADER_STAGE_TRACK_BASE = uint64{1} << 30;
constexpr float64 NANOSECONDS_PER_MICROSECOND = 1000;
uint64 gCapture = 0;
uint64 gNextViewportId = 0;
uint64 gNextCandidateId = 0;
bool isRecording = false;
ViewportTraceSnapshot gSnapshot;
const char* GetLaneName(ViewportTraceLane lane) noexcept
{
    switch (lane)
    {
        case ViewportTraceLane::Frame:
            return "Frame to image ready";
        case ViewportTraceLane::Latency:
            return "Frame to Qt paint";
        case ViewportTraceLane::Stage:
            return "Observed frame stages";
        case ViewportTraceLane::Scheduling:
            return "Between preview services";
        case ViewportTraceLane::Presentation:
            return "Qt presentation wait";
        case ViewportTraceLane::Paint:
            return "Qt paint handler";
        case ViewportTraceLane::Interval:
            return "Paint intervals";
        case ViewportTraceLane::Edit:
            return "Shader edit to first paint";
        case ViewportTraceLane::Candidate:
            return "Graph replacement";
        case ViewportTraceLane::ShaderStage:
            return "Shader build stage";
        case ViewportTraceLane::Count:
            break;
    }
    return "Unknown";
}
int64 GetTrackId(ViewportTraceIdentity identity, ViewportTraceLane lane) noexcept
{
    if (lane == ViewportTraceLane::ShaderStage)
    {
        return static_cast<int64>(SHADER_STAGE_TRACK_BASE + identity.ShaderStage);
    }
    return static_cast<int64>(identity.ViewportId * static_cast<uint64>(ViewportTraceLane::Count) +
                              static_cast<uint64>(lane));
}
bool IsPending(ViewportTraceToken token) noexcept
{
    return isRecording == true && token.Capture == gCapture && token.Index < gSnapshot.Spans.GetSize() &&
           gSnapshot.Spans[token.Index].EndTicks == 0;
}
} // namespace
bool StartViewportTrace() noexcept
{
    if (LUDUS_PROFILING_ENABLED == 0 || isRecording == true)
    {
        return false;
    }
    gSnapshot = {};
    if (gSnapshot.Spans.TryEnsureCapacity(MAX_VIEWPORT_TRACE_SPANS) == false)
    {
        return false;
    }
    ++gCapture;
    gSnapshot.StartedTicks = profiling::NowTicks();
    isRecording = true;
    return true;
}
ViewportTraceSnapshot StopViewportTrace() noexcept
{
    if (isRecording == false)
    {
        return {};
    }
    isRecording = false;
    gSnapshot.StoppedTicks = profiling::NowTicks();
    for (ViewportTraceSpan& span : gSnapshot.Spans)
    {
        if (span.EndTicks == 0)
        {
            span.EndTicks = gSnapshot.StoppedTicks;
            span.Outcome = "capture stopped while pending";
        }
    }
    return Move(gSnapshot);
}
ViewportTraceToken
BeginViewportTrace(const char* name, ViewportTraceLane lane, ViewportTraceIdentity identity, uint64 beginTicks) noexcept
{
    if (isRecording == false)
    {
        return {};
    }
    LUDUS_REQUIRE(name != nullptr);
    if (gSnapshot.Spans.GetSize() == MAX_VIEWPORT_TRACE_SPANS)
    {
        ++gSnapshot.DroppedSpans;
        return {};
    }
    const uint64 now = profiling::NowTicks();
    const uint64 begin = beginTicks == 0 ? now : beginTicks;
    const ViewportTraceSpan span
    {
        .Name = name,
        .Outcome = begin < gSnapshot.StartedTicks ? "capture started during interval" : "complete",
        .Identity = identity,
        .Lane = lane,
        .BeginTicks = begin < gSnapshot.StartedTicks ? gSnapshot.StartedTicks : begin,
    };
    const usize index = gSnapshot.Spans.GetSize();
    if (gSnapshot.Spans.TryAdd(span) == false)
    {
        ++gSnapshot.DroppedSpans;
        return {};
    }
    return {gCapture, index};
}
void EndViewportTrace(ViewportTraceToken token, const char* outcomeOrNull) noexcept
{
    if (IsPending(token) == false)
    {
        return;
    }
    ViewportTraceSpan& span = gSnapshot.Spans[token.Index];
    span.EndTicks = profiling::NowTicks();
    if (outcomeOrNull != nullptr)
    {
        span.Outcome = outcomeOrNull;
    }
}
GraphEditTrace BeginGraphEditTrace() noexcept
{
    if (isRecording == false)
    {
        return {};
    }
    GraphEditTrace edit{++gNextCandidateId};
    edit.Lifetime = BeginViewportTrace("Graph edit to first Qt paint", ViewportTraceLane::Edit, { .Edit = edit });
    return edit;
}
ViewportTraceToken BeginGraphEditStage(GraphEditTrace edit, const char* name) noexcept
{
    return IsPending(edit.Lifetime) == true ? BeginViewportTrace(name, ViewportTraceLane::Candidate, { .Edit = edit })
                                            : ViewportTraceToken{};
}
void RecordShaderStageTrace(GraphEditTrace edit,
                            uint64 stage,
                            const char* name,
                            uint64 beginTicks,
                            uint64 endTicks) noexcept
{
    if (IsPending(edit.Lifetime) == false || endTicks < gSnapshot.StartedTicks || beginTicks > endTicks)
    {
        return;
    }
    const ViewportTraceToken token =
        BeginViewportTrace(name, ViewportTraceLane::ShaderStage, { .Edit = edit, .ShaderStage = stage }, beginTicks);
    if (IsPending(token) == true)
    {
        gSnapshot.Spans[token.Index].EndTicks = endTicks;
    }
}
void AppendViewportTrace(QJsonObject& inoutTrace, const ViewportTraceSnapshot& snapshot)
{
    if (snapshot.StartedTicks == 0)
    {
        return;
    }
    QJsonArray events = inoutTrace.value(QStringLiteral("traceEvents")).toArray();
    events.append(QJsonObject{
        {QStringLiteral("pid"), VIEWPORT_PROCESS_ID},
        {QStringLiteral("ph"), QStringLiteral("M")},
        {QStringLiteral("name"), QStringLiteral("process_name")},
        {QStringLiteral("args"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Editor viewport wall time")}}},
    });
    QJsonArray tracks;
    for (const ViewportTraceSpan& span : snapshot.Spans)
    {
        const int64 trackId = GetTrackId(span.Identity, span.Lane);
        if (tracks.contains(QJsonValue(static_cast<qint64>(trackId))) == false)
        {
            tracks.append(static_cast<qint64>(trackId));
            events.append(QJsonObject{
                {QStringLiteral("pid"), VIEWPORT_PROCESS_ID},
                {QStringLiteral("tid"), static_cast<qint64>(trackId)},
                {QStringLiteral("ph"), QStringLiteral("M")},
                {QStringLiteral("name"), QStringLiteral("thread_name")},
                {QStringLiteral("args"),
                 QJsonObject{{QStringLiteral("name"),
                              span.Lane == ViewportTraceLane::ShaderStage
                                  ? QStringLiteral("Shader build stage %1").arg(span.Identity.ShaderStage)
                                  : QStringLiteral("Viewport %1 / %2")
                                        .arg(span.Identity.ViewportId)
                                        .arg(QString::fromUtf8(GetLaneName(span.Lane)))}}},
            });
        }
        events.append(QJsonObject{
            {QStringLiteral("pid"), VIEWPORT_PROCESS_ID},
            {QStringLiteral("tid"), static_cast<qint64>(trackId)},
            {QStringLiteral("ph"), QStringLiteral("X")},
            {QStringLiteral("cat"), QStringLiteral("editor.viewport.wall")},
            {QStringLiteral("name"), QString::fromUtf8(span.Name)},
            {QStringLiteral("ts"), static_cast<float64>(span.BeginTicks) / NANOSECONDS_PER_MICROSECOND},
            {QStringLiteral("dur"),
             static_cast<float64>(span.EndTicks - span.BeginTicks) / NANOSECONDS_PER_MICROSECOND},
            {QStringLiteral("args"),
             QJsonObject{
                 {QStringLiteral("viewportId"), static_cast<qint64>(span.Identity.ViewportId)},
                 {QStringLiteral("frameId"), static_cast<qint64>(span.Identity.FrameId)},
                 {QStringLiteral("deviceSession"), static_cast<qint64>(span.Identity.DeviceSession)},
                 {QStringLiteral("candidateId"), static_cast<qint64>(span.Identity.Edit.CandidateId)},
                 {QStringLiteral("graphGeneration"), static_cast<qint64>(span.Identity.GraphGeneration)},
                 {QStringLiteral("shaderStage"), static_cast<qint64>(span.Identity.ShaderStage)},
                 {QStringLiteral("outcome"), QString::fromUtf8(span.Outcome)},
             }},
        });
    }
    inoutTrace.insert(QStringLiteral("traceEvents"), events);
    inoutTrace.insert(
        QStringLiteral("ludusViewportTiming"),
        QJsonObject{
            {QStringLiteral("clockRelationship"),
             QStringLiteral("CPU monotonic wall time; waits end when serviced, paint ends before compositor/scanout")},
            {QStringLiteral("spanCount"), static_cast<qint64>(snapshot.Spans.GetSize())},
            {QStringLiteral("droppedSpans"), static_cast<qint64>(snapshot.DroppedSpans)},
            {QStringLiteral("capacity"), static_cast<qint64>(MAX_VIEWPORT_TRACE_SPANS)},
        });
}
ViewportFrameTrace::ViewportFrameTrace() noexcept
{
    mIdentity.ViewportId = ++gNextViewportId;
}
ViewportFrameTrace::~ViewportFrameTrace()
{
    Cancel("preview destroyed");
}
void ViewportFrameTrace::BeginService(uint64 nextFrameId, uint64 deviceSession) noexcept
{
    if (isRecording == true && mGap.Capture != gCapture && mServiceEndTicks != 0)
    {
        mGap = BeginViewportTrace(mStageName, ViewportTraceLane::Scheduling, mIdentity, mServiceEndTicks);
    }
    EndViewportTrace(mGap);
    mServiceBeginTicks = profiling::NowTicks();
    if (mHasFrame == false)
    {
        mIdentity.FrameId = nextFrameId;
        mIdentity.DeviceSession = deviceSession;
        mIdentity.Edit = {};
        mIdentity.GraphGeneration = 0;
    }
    else if (mFrame.Capture != gCapture && isRecording == true)
    {
        // A capture may begin while the renderer or readback is already in flight.
        mFrame = BeginViewportTrace("Frame to image ready", ViewportTraceLane::Frame, mIdentity, mFrameBeginTicks);
        mStage = BeginViewportTrace(mStageName, ViewportTraceLane::Stage, mIdentity, mFrameBeginTicks);
    }
}
void ViewportFrameTrace::EndService() noexcept
{
    mServiceEndTicks = profiling::NowTicks();
    mGap = BeginViewportTrace(mStageName, ViewportTraceLane::Scheduling, mIdentity, mServiceEndTicks);
}
void ViewportFrameTrace::BeginFrame(GraphEditTrace edit, uint64 graphGeneration) noexcept
{
    if (mHasFrame == true)
    {
        SetStage("CPU graph preparation and submission");
        return;
    }
    mHasFrame = true;
    mIdentity.Edit = edit;
    mIdentity.GraphGeneration = graphGeneration;
    mFrameBeginTicks = mServiceBeginTicks;
    mFrame = BeginViewportTrace("Frame to image ready", ViewportTraceLane::Frame, mIdentity, mFrameBeginTicks);
    EndViewportTrace(mStage);
    mStageName = "CPU graph preparation and submission";
    mStage = BeginViewportTrace(mStageName, ViewportTraceLane::Stage, mIdentity, mFrameBeginTicks);
}
void ViewportFrameTrace::SetStage(const char* name) noexcept
{
    LUDUS_REQUIRE(name != nullptr);
    EndViewportTrace(mStage);
    mStageName = name;
    mStage = BeginViewportTrace(name, ViewportTraceLane::Stage, mIdentity);
}
void ViewportFrameTrace::Publish() noexcept
{
    EndViewportTrace(mStage);
    EndViewportTrace(mFrame);
    mPublishedIdentity = mIdentity;
    mPublishedBeginTicks = mFrameBeginTicks;
    mHasFrame = false;
    mStageName = "Await next preview service";
}
void ViewportFrameTrace::Cancel(const char* outcome) noexcept
{
    LUDUS_REQUIRE(outcome != nullptr);
    EndViewportTrace(mStage, outcome);
    EndViewportTrace(mFrame, outcome);
    EndViewportTrace(mGap, outcome);
    mHasFrame = false;
    mStageName = "Preview idle or unavailable";
}
ViewportTraceIdentity ViewportFrameTrace::GetPublishedIdentity() const noexcept
{
    return mPublishedIdentity;
}
uint64 ViewportFrameTrace::GetPublishedBeginTicks() const noexcept
{
    return mPublishedBeginTicks;
}
ScopedViewportService::ScopedViewportService(ViewportFrameTrace& inoutTrace,
                                             uint64 nextFrameId,
                                             uint64 deviceSession) noexcept
    : mTrace(inoutTrace)
{
    mTrace.BeginService(nextFrameId, deviceSession);
}
ScopedViewportService::~ScopedViewportService()
{
    mTrace.EndService();
}
ViewportPaintTrace::~ViewportPaintTrace()
{
    Cancel();
}
void ViewportPaintTrace::Queue(ViewportTraceIdentity identity, uint64 frameBeginTicks) noexcept
{
    EndViewportTrace(mWait, "superseded before paint");
    EndViewportTrace(mLatency, "superseded before paint");
    if (mIdentity.DeviceSession != identity.DeviceSession || mIdentity.ViewportId != identity.ViewportId)
    {
        EndViewportTrace(mInterval, "presentation source replaced");
        mInterval = {};
    }
    mIdentity = identity;
    mQueuedTicks = profiling::NowTicks();
    mFrameBeginTicks = frameBeginTicks;
    mIsQueued = true;
    mWait = BeginViewportTrace("Image ready to Qt paint", ViewportTraceLane::Presentation, identity);
    mLatency = BeginViewportTrace("Frame to Qt paint complete", ViewportTraceLane::Latency, identity, frameBeginTicks);
}
void ViewportPaintTrace::BeginPaint() noexcept
{
    if (mIsQueued == true)
    {
        if (isRecording == true && mWait.Capture != gCapture)
        {
            mWait =
                BeginViewportTrace("Image ready to Qt paint", ViewportTraceLane::Presentation, mIdentity, mQueuedTicks);
            mLatency = BeginViewportTrace("Frame to Qt paint complete",
                                          ViewportTraceLane::Latency,
                                          mIdentity,
                                          mFrameBeginTicks);
        }
        EndViewportTrace(mWait);
        mPaint = BeginViewportTrace("Qt paint image", ViewportTraceLane::Paint, mIdentity);
    }
}
void ViewportPaintTrace::EndPaint() noexcept
{
    if (mIsQueued == true)
    {
        EndViewportTrace(mPaint);
        EndViewportTrace(mLatency);
        if (IsPending(mIdentity.Edit.Lifetime) == true)
        {
            ViewportTraceSpan& edit = gSnapshot.Spans[mIdentity.Edit.Lifetime.Index];
            edit.Identity = mIdentity;
            EndViewportTrace(mIdentity.Edit.Lifetime, "first matching image painted");
        }
        if (isRecording == true && mInterval.Capture == gCapture && mInterval.Index < gSnapshot.Spans.GetSize())
        {
            // Attribute a completed paint interval to the image at its end.
            gSnapshot.Spans[mInterval.Index].Identity = mIdentity;
        }
        EndViewportTrace(mInterval);
        mInterval = BeginViewportTrace("Paint-to-paint frame interval", ViewportTraceLane::Interval, mIdentity);
        mIsQueued = false;
    }
}
void ViewportPaintTrace::Cancel() noexcept
{
    EndViewportTrace(mPaint, "view cleared or destroyed");
    EndViewportTrace(mWait, "view cleared or destroyed");
    EndViewportTrace(mLatency, "view cleared or destroyed");
    EndViewportTrace(mInterval, "view cleared or destroyed");
    mIsQueued = false;
}
} // namespace ludus::editor
