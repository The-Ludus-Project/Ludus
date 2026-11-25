#include "internal/application.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/pack.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/profiling/memory_gauges.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/frame_pacing.h>
#include <string_view>
namespace
{
using ludus::foundation::StaticArray;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::world_demo::LEVEL_TEXT_CAPACITY_BYTES;
using LevelBytes = StaticArray<char, LEVEL_TEXT_CAPACITY_BYTES>;
namespace filesystem = ludus::foundation::filesystem;
namespace timing = ludus::foundation::time;

struct LevelReadTiming
{
    timing::Timestamp OpenStart;
    timing::Timestamp ReadStart;
    timing::Timestamp ReadEnd;
    timing::Timestamp ValidationEnd;
};

// Names the two paths of one --level/--level-pack request so callers cannot swap them.
struct LevelSourcePaths
{
    // Native file: the level text, or the pack that contains it.
    std::string_view NativePath;
    // Path inside the pack; empty when NativePath is the level text itself.
    std::string_view PackedPath;
};

bool ReadLevelSource(const LevelSourcePaths& paths,
                     LevelBytes& outBytes,
                     usize& outSize,
                     LevelReadTiming& outTiming) noexcept
{
    const std::string_view nativePath = paths.NativePath;
    const std::string_view packedPath = paths.PackedPath;
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    const usize separator = nativePath.find_last_of("/\\");
#else
    const usize separator = nativePath.find_last_of('/');
#endif
    const std::string_view root =
        separator == std::string_view::npos ? std::string_view(".") : nativePath.substr(0, separator + 1);
    const std::string_view leaf = separator == std::string_view::npos ? nativePath : nativePath.substr(separator + 1);
    outTiming.OpenStart = timing::Now();
    filesystem::ProviderHandle storage;
    if (filesystem::CreateDirectoryProvider(root, storage).Succeeded() == false)
    {
        return false;
    }
    if (packedPath.empty() == false)
    {
        filesystem::ProviderHandle pack;
        if (filesystem::CreatePackProvider(storage, leaf, {}, pack).Succeeded() == false)
        {
            return false;
        }
        storage = ludus::foundation::Move(pack);
    }
    const filesystem::Provider* providerOrNull = storage.Get();
    filesystem::ProviderFile* fileOrNull = nullptr;
    const std::string_view path = packedPath.empty() == true ? leaf : packedPath;
    if (providerOrNull == nullptr || providerOrNull->OpenRead(path, fileOrNull).Succeeded() == false)
    {
        return false;
    }
    ludus::foundation::core::UniquePtr<filesystem::ProviderFile> file(fileOrNull);
    const ludus::foundation::uint64 size = file->Size();
    if (size == 0 || size > LEVEL_TEXT_CAPACITY_BYTES)
    {
        return false;
    }
    const usize byteCount = static_cast<usize>(size);
    outTiming.ReadStart = timing::Now();
    const filesystem::ReadResult read = file->ReadAt(0, {reinterpret_cast<uint8*>(outBytes.GetData()), byteCount});
    outTiming.ReadEnd = timing::Now();
    if (read.Outcome.Succeeded() == false || read.BytesRead != byteCount)
    {
        return false;
    }
    outSize = byteCount;
    return true;
}

#if LUDUS_TARGET_OS != LUDUS_OS_WEB
bool LogStartupReport([[maybe_unused]] const ludus::world_demo::Application& application,
                      [[maybe_unused]] const LevelReadTiming& readTiming,
                      [[maybe_unused]] timing::Timestamp startupStart,
                      [[maybe_unused]] usize size,
                      [[maybe_unused]] std::string_view entry) noexcept
{
    if (application.GetPresentedFrames() != 1)
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                        "Packed World startup ended before its first presentation.");
        return false;
    }
    LUDUS_LOG_INFO(ludus::foundation::logging::LOG_CORE,
                   "FS-R2 startup: open_start_ns={} read_start_ns={} read_end_ns={} validation_end_ns={} "
                   "world_ready_ns={} renderer_ready_ns={} present_ns={} load_request_ns={} quad_ready_ns={} "
                   "offset=0 bytes={} entry={}",
                   readTiming.OpenStart.Nanoseconds - startupStart.Nanoseconds,
                   readTiming.ReadStart.Nanoseconds - startupStart.Nanoseconds,
                   readTiming.ReadEnd.Nanoseconds - startupStart.Nanoseconds,
                   readTiming.ValidationEnd.Nanoseconds - startupStart.Nanoseconds,
                   application.GetFirstWorldActivation().Nanoseconds - startupStart.Nanoseconds,
                   application.GetFirstRendererReady().Nanoseconds - startupStart.Nanoseconds,
                   application.GetLastPresentReturn().Nanoseconds - startupStart.Nanoseconds,
                   application.GetInitialLoadRequestDuration().Nanoseconds,
                   application.GetQuadReadyDuration().Nanoseconds,
                   size,
                   entry);
    return true;
}
#endif

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
// Thanks to Milos Tosic and the MTuner contributors for correlating memory behavior with a timeline
// (https://github.com/RudjiGames/MTuner#features). Use Ludus domain samples at run boundaries here.
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
    (void)ludus::foundation::profiling::RecordMemoryGauges();
    return true;
}
bool WriteRequestedTrace(std::string_view traceOutput) noexcept
{
    if (traceOutput.empty() == true)
    {
        return true;
    }
    (void)ludus::foundation::profiling::RecordMemoryGauges();
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
// The status mode follows the game flow: a held world that is loading its replacement is not paused for the player.
ludus::world_demo::Mode GetPresentedMode(const ludus::world_demo::Session& session) noexcept
{
    using ludus::world_demo::FlowState;
    using ludus::world_demo::Mode;
    const FlowState state = session.GetFlowState();
    if (state == FlowState::Faulted || session.Driver.GetMode() == Mode::Faulted)
    {
        return Mode::Faulted;
    }
    return state == FlowState::Paused ? Mode::Paused : Mode::Playing;
}
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
            static_cast<ludus::foundation::uint32>(GetPresentedMode(session)),
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
    [[maybe_unused]] const timing::Timestamp startupStart = timing::Now();
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
    // Static storage: the 64 KiB buffer on main's frame left the browser build's
    // 256 KiB stack too little for Application::Start and overflowed it.
    static LevelBytes levelBytes;
    usize levelSize = 0;
    LevelReadTiming levelReadTiming;
    std::string_view startupPackEntry;
    bool isStartupReported = false;
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
        else if (flag == "--startup-report")
        {
            // The build's log level is a constant; `if constexpr` with an else keeps MSVC's
            // constant-condition (C4127) and unreachable-code (C4702) warnings quiet.
            if constexpr (LUDUS_COMPILED_LOG_LEVEL > LUDUS_LOG_LEVEL_INFO)
            {
                LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                                "--startup-report requires a build that includes info logging.");
                return 1;
            }
            else
            {
                isStartupReported = true;
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
        else if ((flag == "--level" || flag == "--level-pack") && argument + 1 < argc)
        {
            const std::string_view path(argv[++argument]);
            std::string_view packedPath;
            if (flag == "--level-pack")
            {
                if (argument + 1 >= argc)
                {
                    return 1;
                }
                packedPath = argv[++argument];
                if (filesystem::ValidPath(packedPath) == false)
                {
                    return 1;
                }
            }
            if (levelSize != 0 || path.empty() == true || path.size() > filesystem::MAX_ROOT_BYTES ||
                ReadLevelSource({ .NativePath = path, .PackedPath = packedPath },
                                levelBytes,
                                levelSize,
                                levelReadTiming) == false)
            {
                LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                                "Cannot read level '{}' from '{}'. Check the path or rebuild the level pack.",
                                packedPath,
                                path);
                return 1;
            }
            Level checked;
            if (ReadLevel({levelBytes.GetData(), levelSize}, checked).Error != LevelError::None)
            {
                LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                                "Invalid level data in '{}'. Validate the authored World and rebuild its pack.",
                                path);
                return 1;
            }
            levelReadTiming.ValidationEnd = timing::Now();
            startupPackEntry = packedPath;
        }
        else
        {
            return 1;
        }
    }
    if (isStartupReported == true &&
        (headless == true || startupPackEntry.empty() == true || frameLimit != 1 || LUDUS_TARGET_OS == LUDUS_OS_WEB))
    {
        LUDUS_LOG_ERROR(ludus::foundation::logging::LOG_CORE,
                        "Use --startup-report with --level-pack and --frames 1 in a native window.");
        return 1;
    }
    const std::string_view source = levelSize != 0 ? std::string_view(levelBytes.GetData(), levelSize) : ExampleLevel();
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
        if (traceOutput.empty() == false)
        {
            (void)ludus::foundation::profiling::RecordMemoryGauges();
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
                          levelSize != 0 ? levelBytes.GetData() : nullptr,
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
    const bool hasStartupReport =
        isStartupReported == false ||
        LogStartupReport(application, levelReadTiming, startupStart, levelSize, startupPackEntry);
    if (traceOutput.empty() == false)
    {
        (void)ludus::foundation::profiling::RecordMemoryGauges();
    }
    application.Shutdown();
    const bool isReportWritten = isPacingReported == false || LogPacingReport(pacingReport) == true;
    const bool isTraceWritten = WriteRequestedTrace(traceOutput);
    return state == AppState::Failed || hasStartupReport == false || isTraceWritten == false || isReportWritten == false
               ? 1
               : 0;
#endif
}
