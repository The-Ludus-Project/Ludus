#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>

class QJsonObject;

namespace ludus::editor
{
/// All operations are GUI-thread only. Storage is reserved before capture; overflow drops spans.
inline constexpr foundation::usize MAX_VIEWPORT_TRACE_SPANS = 32768;
struct ViewportTraceIdentity final
{
    foundation::uint64 ViewportId = 0;
    foundation::uint64 FrameId = 0;
    foundation::uint64 DeviceSession = 0;
};
struct ViewportTraceToken final
{
    foundation::uint64 Capture = 0;
    foundation::usize Index = 0;
};
enum class ViewportTraceLane : foundation::uint8
{
    Frame,
    Latency,
    Stage,
    Scheduling,
    Presentation,
    Paint,
    Interval,
    Count,
};
struct ViewportTraceSpan final
{
    const char* Name = "";
    const char* Outcome = "complete";
    ViewportTraceIdentity Identity;
    ViewportTraceLane Lane = ViewportTraceLane::Stage;
    foundation::uint64 BeginTicks = 0;
    foundation::uint64 EndTicks = 0;
};
struct ViewportTraceSnapshot final
{
    foundation::Array<ViewportTraceSpan> Spans;
    foundation::uint64 DroppedSpans = 0;
    foundation::uint64 StartedTicks = 0;
    foundation::uint64 StoppedTicks = 0;
};
/// Start bounded editor wall-clock recording independently of GPU query ownership.
[[nodiscard]] bool StartViewportTrace() noexcept;
/// Close outstanding spans at the capture boundary and transfer their storage to the caller.
[[nodiscard]] ViewportTraceSnapshot StopViewportTrace() noexcept;
/// Append frozen CPU-clock spans to an already serialized trace on the export worker.
void AppendViewportTrace(QJsonObject& inoutTrace, const ViewportTraceSnapshot& snapshot);
/// Name and nonnull outcome strings must have static lifetime through snapshot serialization.
[[nodiscard]] ViewportTraceToken BeginViewportTrace(const char* name,
                                                    ViewportTraceLane lane,
                                                    ViewportTraceIdentity identity,
                                                    foundation::uint64 beginTicks = 0) noexcept;
void EndViewportTrace(ViewportTraceToken token, const char* outcomeOrNull = nullptr) noexcept;

/// Tracks the graph's observed states across service calls without changing scheduling.
class ViewportFrameTrace final
{
public:
    ViewportFrameTrace() noexcept;
    ~ViewportFrameTrace();
    ViewportFrameTrace(const ViewportFrameTrace&) = delete;
    ViewportFrameTrace& operator=(const ViewportFrameTrace&) = delete;
    void BeginService(foundation::uint64 nextFrameId, foundation::uint64 deviceSession) noexcept;
    void EndService() noexcept;
    void BeginFrame() noexcept;
    void SetStage(const char* name) noexcept;
    void Publish() noexcept;
    void Cancel(const char* outcome) noexcept;
    [[nodiscard]] ViewportTraceIdentity GetPublishedIdentity() const noexcept;
    [[nodiscard]] foundation::uint64 GetPublishedBeginTicks() const noexcept;

private:
    ViewportTraceIdentity mIdentity;
    ViewportTraceIdentity mPublishedIdentity;
    ViewportTraceToken mFrame;
    ViewportTraceToken mStage;
    ViewportTraceToken mGap;
    foundation::uint64 mServiceBeginTicks = 0;
    foundation::uint64 mServiceEndTicks = 0;
    foundation::uint64 mFrameBeginTicks = 0;
    foundation::uint64 mPublishedBeginTicks = 0;
    bool mHasFrame = false;
    const char* mStageName = "Await next preview service";
};
/// Pairs service entry/exit even when preparation fails or the preview is idle.
class ScopedViewportService final
{
public:
    ScopedViewportService(ViewportFrameTrace& inoutTrace,
                          foundation::uint64 nextFrameId,
                          foundation::uint64 deviceSession) noexcept;
    ~ScopedViewportService();
    ScopedViewportService(const ScopedViewportService&) = delete;
    ScopedViewportService& operator=(const ScopedViewportService&) = delete;

private:
    ViewportFrameTrace& mTrace;
};
/// Per-widget presentation tracking. A paint ends at Qt's paint handler, not display scanout.
class ViewportPaintTrace final
{
public:
    ViewportPaintTrace() = default;
    ~ViewportPaintTrace();
    ViewportPaintTrace(const ViewportPaintTrace&) = delete;
    ViewportPaintTrace& operator=(const ViewportPaintTrace&) = delete;
    void Queue(ViewportTraceIdentity identity, foundation::uint64 frameBeginTicks) noexcept;
    void BeginPaint() noexcept;
    void EndPaint() noexcept;
    void Cancel() noexcept;

private:
    ViewportTraceIdentity mIdentity;
    ViewportTraceToken mWait;
    ViewportTraceToken mPaint;
    ViewportTraceToken mLatency;
    ViewportTraceToken mInterval;
    foundation::uint64 mQueuedTicks = 0;
    foundation::uint64 mFrameBeginTicks = 0;
    bool mIsQueued = false;
};
} // namespace ludus::editor
