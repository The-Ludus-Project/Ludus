#include "internal/application.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/input/actions.h>
#include <ludus/platform/keyboard_sink.h>
namespace ludus::world_demo
{
namespace
{
namespace rhi = graphics::rhi;
using input::Key;
void Record(void* context, const input::KeyboardRecord& record) noexcept
{
    (void)static_cast<input::InputSystem*>(context)->Ingest(record);
}
void Reset(void* context, input::ResetReason reason, const input::FocusBaseline& baseline) noexcept
{
    static_cast<input::InputSystem*>(context)->RequestReset(reason, baseline);
}
Key MapKey(platform::browser::Key key) noexcept
{
    using BrowserKey = platform::browser::Key;
    switch (key)
    {
        case BrowserKey::KeyW:
            return Key::KeyW;
        case BrowserKey::KeyA:
            return Key::KeyA;
        case BrowserKey::KeyS:
            return Key::KeyS;
        case BrowserKey::KeyD:
            return Key::KeyD;
        case BrowserKey::Space:
            return Key::Space;
        case BrowserKey::KeyP:
            return Key::KeyP;
        case BrowserKey::KeyN:
            return Key::KeyN;
        case BrowserKey::KeyR:
            return Key::KeyR;
        default:
            return Key::Unknown;
    }
}
} // namespace
bool Application::Start(rhi::BackendSelection selection, const char* levelBytes, usize levelSize) noexcept
{
    Shutdown();
    mSession = Session{};
    mFrame = {};
    mInputFrame = 0;
    mPresentedFrames = 0;
    mPacedPresentedFrames = 0;
    mPresentReturn = {};
    mPacing.Reset();
    mHadFocus = false;
    if (mInput.IsValid() == false)
    {
        mState = AppState::Failed;
        return false;
    }
    const input::Binding bindings[] = {
        { .Action = 0, .Kind = input::ActionKind::Axis1D, .PhysicalKey = Key::KeyD },
        {
            .Action = 0,
            .Kind = input::ActionKind::Axis1D,
            .Direction = input::AxisDirection::Negative,
            .PhysicalKey = Key::KeyA,
        },
        { .Action = 1, .Kind = input::ActionKind::Axis1D, .PhysicalKey = Key::KeyW },
        {
            .Action = 1,
            .Kind = input::ActionKind::Axis1D,
            .Direction = input::AxisDirection::Negative,
            .PhysicalKey = Key::KeyS,
        },
        { .Action = 2, .PhysicalKey = Key::Space },
        { .Action = 3, .PhysicalKey = Key::KeyP },
        { .Action = 4, .PhysicalKey = Key::KeyN },
        { .Action = 5, .PhysicalKey = Key::KeyR },
    };
    if (mInput.ReplaceBindings({bindings}) != input::BindingStatus::Ok)
    {
        mState = AppState::Failed;
        return false;
    }
    platform::WindowManager manager;
    if (manager.Initialize({}) == false ||
        manager.CreateWindow({ .Name = "Ludus world reference", .Width = 960, .Height = 640 }, mWindow) == false)
    {
        mState = AppState::Failed;
        return false;
    }
    mWindow->AttachKeyboardSink({ .OnRecord = Record, .OnReset = Reset, .UserData = &mInput });
    rhi::DeviceDescription description;
    description.Selection = selection;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus status = rhi::CreateDevice({ .Name = "World reference", .Version = 1 },
                                                       mWindow->GetNativeWindowInfo(),
                                                       description,
                                                       mDevice,
                                                       mSurface);
    mHasDevice = status == rhi::DeviceStatus::Ready || status == rhi::DeviceStatus::Pending;
    if (mHasDevice == false)
    {
        Shutdown();
        mState = AppState::Failed;
        return false;
    }
    if (mSession.RequestLoad(levelBytes != nullptr ? std::string_view(levelBytes, levelSize) : ExampleLevel()).Error !=
        LevelError::None)
    {
        Shutdown();
        mState = AppState::Failed;
        return false;
    }
    mState = AppState::Loading;
    return true;
}
void Application::Shutdown() noexcept
{
    // Renderer handles must be released before the device is destroyed.
    mFrameRenderer.Reset();
    mIsRendererReady = false;
    if (mHasDevice == true)
    {
        (void)rhi::DestroyDevice(mDevice);
        mHasDevice = false;
    }
    mDevice = {};
    mSurface = {};
    mWindow.Reset();
    mState = AppState::Stopped;
    mInput.RequestReset(input::ResetReason::FocusLost, {});
}
void Application::browserInput(const platform::browser::WindowState& window) noexcept
{
    const bool focused = window.Focused && window.Visible;
    if (focused != mHadFocus)
    {
        input::FocusBaseline baseline;
        baseline.Focused = focused;
        for (usize index = 1; index < static_cast<usize>(platform::browser::Key::Count); ++index)
        {
            const auto mapped = MapKey(static_cast<platform::browser::Key>(index));
            if (mapped != Key::Unknown)
            {
                baseline.Keys[input::KeyIndex(mapped)] = window.Keys[index];
            }
        }
        mInput.RequestReset(focused ? input::ResetReason::FocusEntered : input::ResetReason::FocusLost, baseline);
        mSession.Driver.Interrupt();
        mHadFocus = focused;
    }
    platform::browser::InputEvent event;
    while (mWindow->PollBrowserInput(event))
    {
        const auto mapped = MapKey(event.PhysicalKey);
        if ((event.Kind == platform::browser::EventKind::KeyDown ||
             event.Kind == platform::browser::EventKind::KeyUp) &&
            mapped != Key::Unknown && focused)
        {
            (void)mInput.Ingest(
            {
                .Transition = event.Kind == platform::browser::EventKind::KeyDown ? input::KeyTransition::Down
                                                                                  : input::KeyTransition::Up,
                .PhysicalKey = mapped,
                .Repeat = event.Repeat,
            });
        }
        if (event.Kind == platform::browser::EventKind::InputReset)
        {
            input::FocusBaseline baseline;
            baseline.Focused = focused;
            for (usize index = 1; index < static_cast<usize>(platform::browser::Key::Count); ++index)
            {
                const auto key = MapKey(static_cast<platform::browser::Key>(index));
                if (key != Key::Unknown)
                {
                    baseline.Keys[input::KeyIndex(key)] = window.Keys[index];
                }
            }
            // Platform queues InputReset when the page is hidden or blurred. A hidden page stops
            // callbacks, so this queued reset also drops the frame baseline and hidden time never
            // becomes catch-up ticks. Visibility events also fire on focus/resize refreshes.
            mInput.RequestReset(focused ? input::ResetReason::ExplicitLive : input::ResetReason::FocusLost, baseline);
            mSession.Driver.Interrupt();
        }
    }
    // Reconcile held states even when the platform event queue overflowed.
    for (usize index = 1; index < static_cast<usize>(platform::browser::Key::Count); ++index)
    {
        const auto mapped = MapKey(static_cast<platform::browser::Key>(index));
        if (mapped != Key::Unknown && focused && mInput.GetLiveDown(mapped) != window.Keys[index])
        {
            (void)mInput.Ingest(
            {
                .Transition = window.Keys[index] ? input::KeyTransition::Down : input::KeyTransition::Up,
                .PhysicalKey = mapped,
            });
        }
    }
}
bool Application::render(const platform::browser::WindowState& window) noexcept
{
    if (rhi::PollLifetime(mDevice) == rhi::RasterStatus::Failed)
    {
        return false;
    }
    const rhi::RasterStatus updated = mFrameRenderer.Update(mFrame, window.FramebufferWidth, window.FramebufferHeight);
    if (updated != rhi::RasterStatus::Ready && updated != rhi::RasterStatus::NotReady)
    {
        // This is a presentation failure, never an authoritative simulation fault.
        return false;
    }
    const rhi::DeviceStatus target = rhi::SetFrameTarget(mDevice,
                                                         mSurface,
                                                         {
                                                             .Width = window.FramebufferWidth,
                                                             .Height = window.FramebufferHeight,
                                                         });
    if (target != rhi::DeviceStatus::Ready)
    {
        return target == rhi::DeviceStatus::Skipped;
    }
    // The viewport opens, draws, tone-maps, encodes and presents the frame in one submission.
    rhi::SubmissionToken completion;
    const rhi::RasterStatus presented = mFrameRenderer.Present(mSurface, completion);
    if (presented != rhi::RasterStatus::Ready)
    {
        return presented == rhi::RasterStatus::NotReady;
    }
    // A diagnostic reading for --pacing-report, independent of the frame's one simulation timestamp.
    mPresentReturn = foundation::time::Now();
    ++mPresentedFrames;
    return true;
}
platform::PaceResult Application::Pace(const platform::PacingClock& clock) noexcept
{
    const bool hasPresented = mPresentedFrames != mPacedPresentedFrames;
    mPacedPresentedFrames = mPresentedFrames;
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
    (void)hasPresented;
    (void)clock;
    return {};
#else
    if (mState == AppState::Stopped || mState == AppState::Failed)
    {
        return {};
    }
    return mPacing.EndFrame(hasPresented, clock);
#endif
}
AppState Application::Frame() noexcept
{
    if (mState == AppState::Stopped || mState == AppState::Failed)
    {
        return mState;
    }
    LUDUS_PROFILE_SCOPE(WorldFrame);
    if (mWindow->HandleEvent({}) == false)
    {
        Shutdown();
        return mState;
    }
    // A minimized or occluded window paces at the hidden rate, also during startup and loading.
    const bool isWindowVisible = mWindow->GetVisibility() == platform::WindowVisibility::Visible;
    mPacing.SetVisible(isWindowVisible);
    // The one frame timestamp; lifecycle resets below apply before the driver samples it.
    const foundation::time::Timestamp now = foundation::time::Now();
    if (mSession.PollLoad() != Status::Success)
    {
        LUDUS_LOG_ERROR(logging::LOG_CORE, "Level candidate failed; active world preserved");
    }
    // Session replacement resets the driver, so the replacement starts from a zero-elapsed baseline.
    // Pacing restarts too: a slow activation frame is not judged against the loading schedule.
    if (mSession.Activate() == true)
    {
        mPacing.Reset();
    }
    rhi::DeviceInfo device;
    const rhi::DeviceStatus deviceStatus = rhi::GetDeviceInfo(mDevice, device);
    if (deviceStatus == rhi::DeviceStatus::Pending)
    {
        return mState;
    }
    if (deviceStatus != rhi::DeviceStatus::Ready)
    {
        Shutdown();
        mState = AppState::Failed;
        return mState;
    }
    const rhi::StartupInfo& startup = device.Startup;
    mSelectedBackend = startup.SelectedBackend;
    const rhi::RasterStatus prepared = mFrameRenderer.Prepare(mDevice);
    mIsRendererReady = prepared == rhi::RasterStatus::Ready;
    if (mIsRendererReady == false && prepared != rhi::RasterStatus::Pending)
    {
        Shutdown();
        mState = AppState::Failed;
        return mState;
    }
    platform::browser::WindowState window;
    const bool browser =
        startup.SelectedBackend == rhi::Backend::WebGPU || startup.SelectedBackend == rhi::Backend::WebGL2;
    if (browser)
    {
        (void)mWindow->SetBrowserFramebufferLimit(startup.MaxTextureDimension2D);
        (void)mWindow->GetBrowserState(window);
        browserInput(window);
    }
    else
    {
        const auto native = mWindow->GetNativeWindowInfo();
        // Like a hidden browser page, a hidden native window drops simulation time instead of ticking.
        window.Visible = isWindowVisible;
        window.FramebufferWidth = native.Width;
        window.FramebufferHeight = native.Height;
    }
    if (mIsRendererReady == false || mSession.World().IsBuilt() == false)
    {
        return mState;
    }
    mState = AppState::Playing;
    if (mInput.ConsumeStep(++mInputFrame) != input::StepStatus::Ok)
    {
        Shutdown();
        mState = AppState::Failed;
        return mState;
    }
    input::ActionState actions[6];
    for (input::ActionId index = 0; index < 6; ++index)
    {
        (void)mInput.GetAction(index, actions[index]);
    }
    // UI owns these frame edges before the gameplay bridge samples its actions.
    if (actions[3].Pressed && mSession.Driver.GetMode() != Mode::Faulted)
    {
        mSession.Driver.SetMode(mSession.Driver.GetMode() == Mode::Playing ? Mode::Paused : Mode::Playing);
    }
    if (actions[4].Pressed && mSession.Driver.GetMode() == Mode::Paused)
    {
        (void)mSession.Driver.Step(mSession.World());
    }
    if (actions[5].Pressed)
    {
        (void)mSession.RequestLoad(ExampleLevel());
    }
    TickInput input
    {
        .Axis = {actions[0].Value, actions[1].Value},
        .Held = actions[2].Value != 0,
        .Pressed = actions[2].Pressed,
        .Released = actions[2].Released,
        .Cancelled = actions[0].Cancelled || actions[1].Cancelled || actions[2].Cancelled,
    };
    if (mSession.Driver.Advance(mSession.World(), now, input, window.Visible) != Status::Success)
    {
        LUDUS_LOG_ERROR(logging::LOG_CORE, "Simulation fault at tick {}", mSession.World().GetTick());
    }
    // Stalls and regressions stay visible in diagnostics even though the simulation discards them.
    const foundation::time::FrameSample& frameSample = mSession.Driver.GetFrameSample();
    if (frameSample.Status == foundation::time::TimeStatus::ClockRegression)
    {
        LUDUS_LOG_WARN(logging::LOG_CORE, "Frame clock regressed; no simulation time accepted");
    }
    else if (frameSample.Discarded.Nanoseconds != 0)
    {
        LUDUS_LOG_INFO(logging::LOG_CORE,
                       "Frame stall discarded {} ns beyond the accepted delta",
                       frameSample.Discarded.Nanoseconds);
    }
    if (mSession.Driver.GetMode() != Mode::Faulted)
    {
        if (mSession.World().Extract(mSession.Driver.GetAlpha(), mFrame, mCameraFollow, mInputFrame) == false)
        {
            LUDUS_LOG_WARN(logging::LOG_CORE, "Camera extraction failed; retaining previous frame");
        }
    }
    // Presentation delivery is independent of image acquisition. This adapter
    // logs copied facts once; audio and particle assets are a later game feature.
    for ([[maybe_unused]] const auto& event : mSession.World().GetOutbox())
    {
        LUDUS_LOG_INFO(logging::LOG_CORE,
                       "World {} tick {} event {}",
                       event.World,
                       event.Tick,
                       static_cast<uint32>(event.Kind));
    }
    mSession.World().ConsumeOutbox();
    if (mSession.World().GetNextLevel().View().empty() == false && mSession.GetLoadStage() == LoadStage::Empty)
    {
        (void)mSession.RequestLoad(BuiltinLevel(mSession.World().GetNextLevel().View()));
    }
    if (render(window) == false)
    {
        Shutdown();
        mState = AppState::Failed;
    }
    LUDUS_PROFILE_FRAME();
    return mState;
}
} // namespace ludus::world_demo
