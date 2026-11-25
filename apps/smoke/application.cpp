#include "internal/application.h"
#include "internal/renderer.h"
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>
namespace ludus::smoke
{
using namespace foundation;
using namespace graphics;
namespace
{
constexpr usize RhiProviders[]{0};
constexpr usize RendererProviders[]{1};
/// Lifecycle error domain of every smoke adapter: values are rhi::StartupError codes.
constexpr uint32 STARTUP_ERROR_DOMAIN = 1;
[[nodiscard]] lifecycle::ErrorCode MakeStartupError(rhi::StartupError error) noexcept
{
    return { .Domain = STARTUP_ERROR_DOMAIN, .Value = static_cast<uint32>(error) };
}
[[nodiscard]] rhi::StartupError GetStartupError(const lifecycle::Failure& failure) noexcept
{
    LUDUS_REQUIRE(failure.Error.Domain == STARTUP_ERROR_DOMAIN);
    return static_cast<rhi::StartupError>(failure.Error.Value);
}
/// Initial size of the demo window, in logical pixels.
constexpr uint32 WINDOW_WIDTH = 800;
constexpr uint32 WINDOW_HEIGHT = 600;
} // namespace
Application::Application() noexcept : mLifecycle(mNodes, mRecords, mJournal)
{
    mNodes[0] =
    {
        .Id = 0,
        .Name = "Window",
        .ContextOrNull = this,
        .Providers = {},
        .BeginStart = StartWindow,
        .PollStart = StartWindow,
        .BeginStop = StopWindow,
        .PollStop = StopWindow,
    };
    mNodes[1] =
    {
        .Id = 1,
        .Name = "RHI",
        .ContextOrNull = this,
        .Providers = RhiProviders,
        .BeginStart = StartRhi,
        .PollStart = PollRhi,
        .BeginStop = StopRhi,
        .PollStop = StopRhi,
    };
    mNodes[2] =
    {
        .Id = 2,
        .Name = "Renderer",
        .ContextOrNull = this,
        .Providers = RendererProviders,
        .BeginStart = PrepareRenderer,
        .PollStart = PrepareRenderer,
        .BeginStop = StopRenderer,
        .PollStop = StopRenderer,
    };
}
Application::~Application()
{
    // Async cleanup must finish while providers and the owner loop still exist.
    LUDUS_REQUIRE(mLifecycle.GetState() == lifecycle::State::Idle ||
                  mLifecycle.GetState() == lifecycle::State::Stopped);
}
lifecycle::StartResult Application::StartWindow(void* context) noexcept
{
    auto& app = *static_cast<Application*>(context);
    platform::WindowManager manager;
    if (manager.Initialize({}) == false || manager.CreateWindow(
                                               {
                                                   .Name = "Ludus demo",
                                                   .Width = WINDOW_WIDTH,
                                                   .Height = WINDOW_HEIGHT,
                                               },
                                               app.mWindow) == false)
    {
        return
        {
            .Status = lifecycle::StartStatus::Failed,
            .Error = MakeStartupError(rhi::StartupError::InvalidWindow),
        };
    }
    return {};
}
lifecycle::StartResult Application::StartRhi(void* context) noexcept
{
    auto& app = *static_cast<Application*>(context);
    const auto result = rhi::Start({ .Name = "Smoke App", .Version = 1 },
                                   app.mWindow->GetNativeWindowInfo(),
                                   app.mSelection,
                                   renderer::Requirements());
    if (result == rhi::StartStatus::Ready || result == rhi::StartStatus::Pending)
    {
        return PollRhi(context);
    }
    return { .Status = lifecycle::StartStatus::Failed, .Error = MakeStartupError(rhi::GetStartup().Error) };
}
lifecycle::StartResult Application::PollRhi(void*) noexcept
{
    const auto startup = rhi::GetStartup();
    if (startup.State == rhi::StartupState::Ready)
    {
        return {};
    }
    if (startup.State == rhi::StartupState::Pending)
    {
        return { .Status = lifecycle::StartStatus::Pending, .WaitReason = 1 };
    }
    return { .Status = lifecycle::StartStatus::Failed, .Error = MakeStartupError(startup.Error) };
}
lifecycle::StartResult Application::PrepareRenderer(void*) noexcept
{
    const auto state = renderer::Prepare();
    if (state == renderer::State::Loading)
    {
        return { .Status = lifecycle::StartStatus::Pending, .WaitReason = 2 };
    }
    if (state == renderer::State::Failed)
    {
        return
        {
            .Status = lifecycle::StartStatus::Failed,
            .Error = MakeStartupError(rhi::StartupError::RenderingUnavailable),
        };
    }
    return {};
}
lifecycle::StopResult Application::StopWindow(void* context) noexcept
{
    static_cast<Application*>(context)->mWindow.Reset();
    return {};
}
lifecycle::StopResult Application::StopRhi(void* context) noexcept
{
    auto& app = *static_cast<Application*>(context);
    const auto startup = rhi::GetStartup();
    app.mWebGpuError = startup.WebGpu.Error;
    app.mWebGl2Error = startup.WebGL2.Error;
    rhi::Shutdown();
    return {};
}
lifecycle::StopResult Application::StopRenderer(void*) noexcept
{
    renderer::Shutdown();
    return {};
}
void Application::Shutdown() noexcept
{
    mLifecycle.RequestStop();
    // These three existing facades retire synchronously. Future Pending stop
    // adapters require an explicit Stopping application state and owner-loop pump.
    const auto stopped = mLifecycle.Advance(NodeCount);
    LUDUS_REQUIRE(stopped == lifecycle::State::Stopped);
    mState = State::Stopped;
    mError = rhi::StartupError::None;
    mFrameClock.Reset();
}
void Application::Fail(State state, rhi::StartupError error, usize nodeId) noexcept
{
    LUDUS_LOG_ERROR(logging::LOG_CORE, "Smoke application stopped: graphics error {}", static_cast<uint32>(error));
    // Capture the bounded per-attempt diagnostics before Shutdown clears the RHI
    // session, so QA metadata (which browser paths were tried, and why) survives.
    const auto startup = rhi::GetStartup();
    mWebGpuError = startup.WebGpu.Error;
    mWebGl2Error = startup.WebGL2.Error;
    (void)mLifecycle.ReportFailure(nodeId, MakeStartupError(error));
    Shutdown();
    mState = state;
    mError = error;
}
bool Application::Start(rhi::BackendSelection selection) noexcept
{
    Shutdown();
    mSimulation = {};
    mFrames = 0;
    mWebGpuError = rhi::StartupError::None;
    mWebGl2Error = rhi::StartupError::None;
    mSelection = selection;
    const auto begin = mLifecycle.Begin();
    // InvalidPlan and Busy are composition-root programming errors here; do not
    // misreport them as GPU availability or generation exhaustion.
    LUDUS_REQUIRE(begin != lifecycle::BeginStatus::InvalidPlan && begin != lifecycle::BeginStatus::Busy);
    if (begin != lifecycle::BeginStatus::Started)
    {
        Fail(State::Failed, rhi::StartupError::GenerationExhausted);
        return false;
    }
    (void)mLifecycle.Advance(NodeCount);
    if (mLifecycle.GetOutcome() == lifecycle::Outcome::StartFailed)
    {
        Fail(State::Failed, GetStartupError(mLifecycle.GetFailure()));
        return false;
    }
    mState = State::Loading;
    // Startup: the first Playing callback establishes a zero-elapsed baseline.
    mFrameClock.Reset();
    return true;
}
State Application::Tick() noexcept
{
    if (mState != State::Loading && mState != State::Playing)
    {
        return mState;
    }
    LUDUS_PROFILE_SCOPE(Frame);
    if (mWindow->HandleEvent({}) == false)
    {
        Shutdown();
        return mState;
    }
    // The one frame timestamp; suspension resets below apply before it is sampled.
    const time::Timestamp now = time::Now();
    if (mState == State::Loading)
    {
        const auto state = mLifecycle.Advance(NodeCount);
        if (mLifecycle.GetOutcome() == lifecycle::Outcome::StartFailed)
        {
            const rhi::StartupError error = GetStartupError(mLifecycle.GetFailure());
            Fail(error == rhi::StartupError::DeviceLost ? State::DeviceLost : State::Failed, error);
            return mState;
        }
        if (state != lifecycle::State::Ready)
        {
            return mState;
        }
        mState = State::Playing;
    }
    const auto startup = rhi::GetStartup();
    if (startup.State == rhi::StartupState::Failed || startup.State == rhi::StartupState::DeviceLost)
    {
        Fail(startup.State == rhi::StartupState::DeviceLost ? State::DeviceLost : State::Failed, startup.Error);
        return mState;
    }
    platform::browser::WindowState input;
    // Browser input/framebuffer handling applies to any browser backend, not
    // just WebGPU. The window being a browser canvas (not the selected graphics
    // backend) is what determines whether to read browser state.
    const bool browser =
        startup.SelectedBackend == rhi::Backend::WebGPU || startup.SelectedBackend == rhi::Backend::WebGL2;
    if (browser)
    {
        (void)mWindow->SetBrowserFramebufferLimit(startup.MaxTextureDimension2D);
        (void)mWindow->GetBrowserState(input);
    }
    else
    {
        input.Visible = true;
    }
    platform::browser::InputEvent event;
    while (mWindow->PollBrowserInput(event))
    {
        // Held keys/buttons and pointer positions come from the snapshot,
        // including after queue overflow, blur and visibility transitions.
        // Platform queues InputReset when the page is hidden or blurred. A
        // hidden page stops callbacks, so the queued reset marks an interval
        // spanning hidden time. Visibility events also fire on focus/resize
        // refreshes and do not distinguish hiding from showing.
        if (event.Kind == platform::browser::EventKind::InputReset)
        {
            mFrameClock.Reset();
        }
    }
    Advance(mSimulation, input, SampleFrame(mFrameClock, now, input.Visible));
    const auto rendered = renderer::Render(input, mSimulation);
    if (rendered == rhi::FrameStatus::Failed || rendered == rhi::FrameStatus::InvalidState)
    {
        const auto failed = rhi::GetStartup();
        Fail(failed.State == rhi::StartupState::DeviceLost ? State::DeviceLost : State::Failed,
             failed.Error == rhi::StartupError::None ? rhi::StartupError::RenderingUnavailable : failed.Error,
             failed.Error == rhi::StartupError::None ? 2 : 1);
        return mState;
    }
    if (rendered == rhi::FrameStatus::Ready)
    {
        ++mFrames;
    }
    LUDUS_PROFILE_FRAME();
    return mState;
}
} // namespace ludus::smoke
