#include "internal/application.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/frame_pacing.h>
#include <string_view>
namespace
{
constexpr ludus::foundation::uint64 HEADLESS_DEFAULT_TICKS = 120;
constexpr ludus::foundation::usize MAX_TRACE_PATH_BYTES = 4096;
// Windowed --pacing-report runs retain records for startup and loading iterations beyond --frames.
constexpr ludus::foundation::usize PACING_REPORT_EXTRA_RECORDS = 1024;
void LogDistribution([[maybe_unused]] std::string_view name,
                     [[maybe_unused]] const ludus::world_demo::IntervalDistribution& distribution) noexcept
{
    LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                   "Pacing {} ns: count={} min={} p50={} p90={} p99={} max={} mean={}",
                   name,
                   distribution.Count,
                   distribution.Minimum,
                   distribution.Median,
                   distribution.Percentile90,
                   distribution.Percentile99,
                   distribution.Maximum,
                   distribution.Mean);
}
// --pacing-report: log the outcome counts and distributions of one run (see the evidence page).
bool LogPacingReport(const ludus::world_demo::PacingReport& report) noexcept
{
    ludus::world_demo::PacingSummary summary;
    if (report.Summarize(summary) == false)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE, "Could not summarize the pacing report");
        return false;
    }
    LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                   "Pacing frames={} waited={} late={} rescheduled={} presentation-paced={} other={} "
                   "early-wakes={} dropped={}",
                   summary.Frames,
                   summary.Waited,
                   summary.Late,
                   summary.Rescheduled,
                   summary.PresentationPaced,
                   summary.Other,
                   summary.EarlyWakes,
                   report.GetDroppedRecords());
    LogDistribution("wake lateness", summary.WakeLateness);
    LogDistribution("frame start interval", summary.FrameStart);
    LogDistribution("present return interval", summary.PresentReturn);
    return true;
}
// --trace-output arms a FoundationProfiling capture for the whole run. A
// profiling-disabled (Release) build cannot honour it, so the request fails
// instead of silently producing no trace for automated profiling.
bool BeginRequestedTrace(std::string_view traceOutput) noexcept
{
    if (traceOutput.empty() == true)
    {
        return true;
    }
    ludus::foundation::profiling::RegisterThreadForTrace("Main");
    if (ludus::foundation::profiling::BeginCapture() == false)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                        "--trace-output requires a profiling-enabled Debug, Development or Profile build");
        return false;
    }
    return true;
}
bool WriteRequestedTrace(std::string_view traceOutput) noexcept
{
    if (traceOutput.empty() == true)
    {
        return true;
    }
    ludus::foundation::profiling::EndCapture();
    // Read only by the info log, which a Release build's log level compiles out.
    [[maybe_unused]] const ludus::foundation::profiling::TraceHealth health =
        ludus::foundation::profiling::GetTraceHealth();
    if (ludus::foundation::profiling::ExportPerfettoTrace(traceOutput) == false)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE, "Could not write profiling trace {}", traceOutput);
        return false;
    }
    LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                   "Wrote profiling trace {}: events={} dropped={}",
                   traceOutput,
                   health.EventsRecorded,
                   health.EventsDropped);
    return true;
}
} // namespace
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
#    include <emscripten.h>
namespace
{
ludus::world_demo::Application gApplication;
// The browser status element receives the placement digest as two 32-bit halves.
constexpr ludus::foundation::uint32 DIGEST_HIGH_SHIFT = 32;
// clang-format off
EM_JS(bool, FollowCameraRequested, (), {
    return new URLSearchParams(location.search).get('camera') === 'follow';
});
EM_JS(void, Present, (ludus::foundation::uint32 state, ludus::foundation::uint32 mode, ludus::foundation::uint32 backend, ludus::foundation::uint32 tick, ludus::foundation::float32 x, ludus::foundation::float32 y, ludus::foundation::uint32 placements, ludus::foundation::uint32 digestHigh, ludus::foundation::uint32 digestLow), {
    const status = document.getElementById('status');
    status.dataset.state = ['stopped','loading','playing','failed'][state];
    status.dataset.mode = ['playing','paused','faulted'][mode];
    status.dataset.backend = ['vulkan','webgpu','webgl2'][backend];
    status.dataset.tick = tick; status.dataset.x = x; status.dataset.y = y;
    status.dataset.placements = placements;
    status.dataset.scatter = (digestHigh >>> 0).toString(16).padStart(8, '0') + (digestLow >>> 0).toString(16).padStart(8, '0'); // ludus-style: magic-number JavaScript hexadecimal formatting of the two 32-bit digest halves
    status.textContent = state == 3 ? 'Graphics could not start. Reload to retry.' : mode == 2 ? 'Simulation stopped. Press R to restart.' : mode == 1 ? 'Paused. N steps one tick; P resumes.' : 'WASD moves. Space attacks. P pauses. R restarts.';
});
// clang-format on
void Frame() noexcept
{
    const auto state = gApplication.Frame();
    const auto& session = gApplication.GetSession();
    const auto player = session.World().FindAuthored("player-start");
    const auto* transform = session.World().Transforms().Find(player);
    // The placement digest lets the browser harness compare the room's scatter with the native fixture.
    const ludus::foundation::uint64 digest = session.World().GetPlacementDigest();
    Present(static_cast<ludus::foundation::uint32>(state),
            static_cast<ludus::foundation::uint32>(session.Driver.GetMode()),
            static_cast<ludus::foundation::uint32>(gApplication.GetSelectedBackend()),
            static_cast<ludus::foundation::uint32>(session.World().GetTick()),
            transform != nullptr ? transform->Current.X : 0,
            transform != nullptr ? transform->Current.Y : 0,
            static_cast<ludus::foundation::uint32>(session.World().GetPlacements().size()),
            static_cast<ludus::foundation::uint32>(digest >> DIGEST_HIGH_SHIFT),
            static_cast<ludus::foundation::uint32>(digest));
}
} // namespace
#endif
int main(int argc, char** argv)
{
    using namespace ludus::world_demo;
    ludus::foundation::logging::LogConfig config;
    config.EnableConsole = true;
    config.EnableFile = false;
    ludus::foundation::logging::LogSystem::Initialize(config);
    // Every explicit error exit must join the logging flush worker too.
    struct LoggerCleanup final
    {
        ~LoggerCleanup() noexcept
        {
            ludus::foundation::logging::LogSystem::Shutdown();
        }
    } cleanup;
    // --level is an explicit bounded read. The same codec validates Editor and
    // runtime; loading never rewrites, builds or repairs the source project.
    char levelBytes[65536] = {};
    usize levelSize = 0;
    bool headless = false;
    bool follow = false;
    uint64 frameLimit = 0;
    uint64 frameRateLimit = 0;
    bool isPacingReported = false;
    std::string_view traceOutput;
    for (int argument = 1; argument < argc; ++argument)
    {
        const std::string_view flag(argv[argument]);
        if (flag == "--headless")
        {
            headless = true;
        }
        else if (flag == "--follow-camera")
        {
            follow = true;
        }
        else if (flag == "--frames" && argument + 1 < argc)
        {
            const std::string_view text(argv[++argument]);
            if (ParseUint64(text.data(), text.size(), frameLimit) != NumberParseStatus::Success || frameLimit == 0)
            {
                return 1;
            }
        }
        else if (flag == "--frame-rate" && argument + 1 < argc)
        {
            // Native frame-rate cap; validated by LoopPacing (1 to MAXIMUM_FRAMES_PER_SECOND).
            const std::string_view text(argv[++argument]);
            if (ParseUint64(text.data(), text.size(), frameRateLimit) != NumberParseStatus::Success ||
                frameRateLimit == 0 || frameRateLimit > MAXIMUM_FRAMES_PER_SECOND)
            {
                return 1;
            }
        }
        else if (flag == "--pacing-report")
        {
            isPacingReported = true;
        }
        else if (flag == "--trace-output" && argument + 1 < argc)
        {
            traceOutput = std::string_view(argv[++argument]);
            if (traceOutput.empty() == true || traceOutput.size() > MAX_TRACE_PATH_BYTES)
            {
                return 1;
            }
        }
        else if (flag == "--level" && argument + 1 < argc)
        {
            const std::string_view path(argv[++argument]);
            if (levelSize != 0 || path.empty() || path.size() > 4096)
            {
                return 1;
            }
            // Split only at this existing filesystem compatibility boundary;
            // Directory owns the root and refuses child symlinks.
            auto slash = std::string_view::npos;
            for (usize i = 0; i < path.size(); ++i)
            {
                if (path[i] == '/')
                {
                    slash = i;
                }
            }
            const auto root = slash == std::string_view::npos ? std::string_view(".")
                              : slash == 0                    ? path.substr(0, 1)
                                                              : path.substr(0, slash);
            const auto leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
            ludus::foundation::filesystem::Directory directory;
            ludus::foundation::filesystem::File file;
            if (directory.Open(root).Succeeded() == false || directory.OpenRead(leaf, file).Succeeded() == false ||
                file.Size() == 0 || file.Size() > sizeof(levelBytes))
            {
                return 1;
            }
            levelSize = static_cast<usize>(file.Size());
            const auto read = file.ReadAt(0, {reinterpret_cast<uint8*>(levelBytes), levelSize});
            if (read.Outcome.Succeeded() == false || read.BytesRead != levelSize)
            {
                return 1;
            }
            Level checked;
            if (ReadLevel({levelBytes, levelSize}, checked).Error != LevelError::None)
            {
                return 1;
            }
        }
        else
        {
            return 1;
        }
    }
    const auto source = levelSize != 0 ? std::string_view(levelBytes, levelSize) : ExampleLevel();
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
    // A browser trace leaves MEMFS only through an explicit page download
    // (trace_system.hpp), which this demo does not implement.
    if (headless == false && traceOutput.empty() == false)
    {
        return 1;
    }
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
    // Browsers pace with presentation callbacks and never wait in the frame loop.
    if (frameRateLimit != 0 || isPacingReported == true)
    {
        return 1;
    }
#endif
    // A report needs a bounded run: --frames sets the presented (or headless) frame count.
    PacingReport pacingReport;
    if (isPacingReported == true && (frameLimit == 0 || pacingReport.Reserve(static_cast<usize>(frameLimit) +
                                                                             PACING_REPORT_EXTRA_RECORDS) == false))
    {
        return 1;
    }
    const ludus::platform::PacingClock pacingClock = ludus::platform::GetSystemPacingClock();
    if (BeginRequestedTrace(traceOutput) == false)
    {
        return 1;
    }
    if (headless)
    {
        Session session;
        if (session.RequestLoad(source).Error != LevelError::None)
        {
            return 1;
        }
        for (usize step = 0; step < 40 && session.GetLoadStage() != LoadStage::Ready; ++step)
        {
            if (session.PollLoad() != Status::Success)
            {
                return 1;
            }
        }
        if (session.Activate() == false)
        {
            return 1;
        }
        // --frames selects the headless tick count; the reference run keeps 120.
        const uint64 tickCount = frameLimit != 0 ? frameLimit : HEADLESS_DEFAULT_TICKS;
        // --frame-rate paces each headless iteration: a graphics-free measurement of operating-system
        // waits. Pacing never changes the ticks run, so the replay hash is unchanged.
        LoopPacing headlessPacing;
        if (frameRateLimit != 0 && headlessPacing.Configure(static_cast<uint32>(frameRateLimit)) == false)
        {
            return 1;
        }
        for (uint64 tick = 0; tick < tickCount; ++tick)
        {
            if (session.World().RunTick({ .Axis = {0.5F, 0} }) != Status::Success)
            {
                return 1;
            }
            LUDUS_PROFILE_FRAME();
            if (headlessPacing.HasLimit() == true)
            {
                const ludus::platform::PaceResult pace = headlessPacing.EndFrame(false, pacingClock);
                if (isPacingReported == true)
                {
                    pacingReport.Record({ .Pace = pace, .PresentReturn = {}, .HasPresented = false });
                }
            }
        }
        if (isPacingReported == true && LogPacingReport(pacingReport) == false)
        {
            return 1;
        }
        LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                       "Headless reference: ticks={} hash={} world-seed={} room={} placements={} scatter={}",
                       session.World().GetTick(),
                       session.World().GetReplayHash(),
                       session.Seed.Value,
                       session.World().GetRoom(),
                       session.World().GetPlacements().size(),
                       session.World().GetPlacementDigest());
        return WriteRequestedTrace(traceOutput) == true ? 0 : 1;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
    (void)follow;
    (void)frameLimit;
    (void)pacingClock;
    gApplication.SetCameraFollow(FollowCameraRequested());
    (void)gApplication.Start();
    emscripten_set_main_loop(Frame, 0, true);
#else
    Application application;
    application.SetCameraFollow(follow);
    if (application.SetFrameRateLimit(static_cast<uint32>(frameRateLimit)) == false)
    {
        return 1;
    }
    if (application.Start(ludus::graphics::rhi::BackendSelection::Auto,
                          levelSize != 0 ? levelBytes : nullptr,
                          levelSize) == false)
    {
        return 1;
    }
    uint64 presentedFrames = 0;
    while (application.Frame() != AppState::Stopped && application.GetState() != AppState::Failed)
    {
        const bool hasPresented = application.GetPresentedFrames() != presentedFrames;
        presentedFrames = application.GetPresentedFrames();
        if (frameLimit != 0 && presentedFrames >= frameLimit)
        {
            break;
        }
        // Wait for the next frame's target: the cap, or the idle rate when nothing was presented.
        const ludus::platform::PaceResult pace = application.Pace(pacingClock);
        if (isPacingReported == true)
        {
            pacingReport.Record(
            {
                .Pace = pace,
                .PresentReturn = application.GetLastPresentReturn(),
                .HasPresented = hasPresented,
            });
        }
    }
    LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                   "Reference presented {} frames",
                   application.GetPresentedFrames());
    const AppState state = application.GetState();
    application.Shutdown();
    const bool isReportWritten = isPacingReported == false || LogPacingReport(pacingReport) == true;
    const bool isTraceWritten = WriteRequestedTrace(traceOutput);
    return state == AppState::Failed || isTraceWritten == false || isReportWritten == false ? 1 : 0;
#endif
}
