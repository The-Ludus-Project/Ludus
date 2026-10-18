#include "internal/application.h"
#include "internal/camera_view.h"
#include "world_scene.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/actions.h>
#include <ludus/platform/keyboard_sink.h>
#include <span>
#include <type_traits>
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
// Independently checked std140/WGSL/SPIR-V: vector array stride 16;
// view/settings at 0/16, geometry/color/rotation at 32/1056/2080, size 3104.
struct alignas(16) Uniforms final
{
    float32 View[4] = {};
    float32 Settings[4] = {};
    float32 Geometry[64][4] = {};
    float32 Color[64][4] = {};
    float32 Rotation[64][4] = {};
};
static_assert(std::is_standard_layout_v<Uniforms> && sizeof(Uniforms) == 3104 && alignof(Uniforms) == 16);
static_assert(offsetof(Uniforms, Settings) == 16 && offsetof(Uniforms, Geometry) == 32 &&
              offsetof(Uniforms, Color) == 1056 && offsetof(Uniforms, Rotation) == 2080);
bool Accepted(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready || status == rhi::ResourceStatus::Pending;
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
    const auto status =
        rhi::Start({ .Name = "World reference", .Version = 1 }, mWindow->GetNativeWindowInfo(), selection);
    if (status != rhi::StartStatus::Ready && status != rhi::StartStatus::Pending)
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
    rhi::Shutdown();
    mWindow.Reset();
    mVertex = {};
    mFragment = {};
    mUniform = {};
    mPipeline = {};
    mCreated = false;
    mRendererReady = false;
    mState = AppState::Stopped;
    mInput.RequestReset(input::ResetReason::FocusLost, {});
}
bool Application::PrepareRenderer() noexcept
{
    if (mCreated == false)
    {
        if (Accepted(rhi::CreateShader(shaders::world_scene::Vertex(), mVertex)) == false ||
            Accepted(rhi::CreateShader(shaders::world_scene::Fragment(), mFragment)) == false ||
            Accepted(rhi::CreateUniform(sizeof(Uniforms), mUniform)) == false)
        {
            return false;
        }
        mCreated = true;
    }
    for (const auto status : {rhi::GetStatus(mVertex), rhi::GetStatus(mFragment), rhi::GetStatus(mUniform)})
    {
        if (status == rhi::ResourceStatus::Pending)
        {
            return true;
        }
        if (status != rhi::ResourceStatus::Ready)
        {
            return false;
        }
    }
    if (rhi::GetStatus(mPipeline) == rhi::ResourceStatus::InvalidHandle)
    {
        if (Accepted(rhi::CreatePipeline({mVertex, mFragment, mUniform}, mPipeline)) == false)
        {
            return false;
        }
    }
    const auto status = rhi::GetStatus(mPipeline);
    mRendererReady = status == rhi::ResourceStatus::Ready;
    return mRendererReady || status == rhi::ResourceStatus::Pending;
}
void Application::BrowserInput(const platform::browser::WindowState& window) noexcept
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
bool Application::Render(const platform::browser::WindowState& window) noexcept
{
    auto status = rhi::SetFrameTarget({ .Width = window.FramebufferWidth, .Height = window.FramebufferHeight });
    if (status == rhi::FrameStatus::Skipped)
    {
        return true;
    }
    if (status != rhi::FrameStatus::Ready)
    {
        return false;
    }
    status = rhi::BeginFrameStatus();
    if (status == rhi::FrameStatus::Skipped)
    {
        return true;
    }
    if (status != rhi::FrameStatus::Ready)
    {
        return false;
    }
    const auto info = rhi::GetFrameInfo();
    PlanarCameraView cameraView;
    if (TryBuildPlanarCameraView(mFrame.Camera,
                                 {
                                     .Width = info.Width,
                                     .Height = info.Height,
                                 },
                                 {},
                                 1.0e6,
                                 cameraView) != gameplay::camera::CameraStatus::Success)
    {
        // This is a presentation failure, never an authoritative simulation fault.
        (void)rhi::EndFrame();
        return false;
    }
    Uniforms uniforms;
    uniforms.View[0] = static_cast<float32>(info.Width);
    uniforms.View[1] = static_cast<float32>(info.Height);
    uniforms.View[2] = cameraView.Center.X;
    uniforms.View[3] = cameraView.Center.Y;
    uniforms.Settings[0] = cameraView.VerticalSpan;
    uniforms.Settings[1] = static_cast<float32>(mFrame.Count);
    for (usize index = 0; index < mFrame.Count; ++index)
    {
        const auto& draw = mFrame.Draws[index];
        uniforms.Geometry[index][0] = draw.Position.X;
        uniforms.Geometry[index][1] = draw.Position.Y;
        uniforms.Geometry[index][2] = draw.HalfExtent.X;
        uniforms.Geometry[index][3] = draw.HalfExtent.Y;
        uniforms.Rotation[index][0] = draw.Rotation;
        for (usize channel = 0; channel < 4; ++channel)
        {
            uniforms.Color[index][channel] = draw.Color[channel];
        }
    }
    const auto bytes = std::span<const uint8>(reinterpret_cast<const uint8*>(&uniforms), sizeof(uniforms));
    if (rhi::UpdateUniform(mUniform, bytes) != rhi::ResourceStatus::Ready ||
        rhi::DrawFullscreen(mPipeline) != rhi::ResourceStatus::Ready)
    {
        return false;
    }
    const auto end = rhi::EndFrameStatus();
    if (end == rhi::FrameStatus::Ready)
    {
        // A diagnostic reading for --pacing-report, independent of the frame's one simulation timestamp.
        mPresentReturn = foundation::time::Now();
        ++mPresentedFrames;
    }
    return end == rhi::FrameStatus::Ready || end == rhi::FrameStatus::Skipped;
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
    const auto startup = rhi::GetStartup();
    if (startup.State == rhi::StartupState::Failed || startup.State == rhi::StartupState::DeviceLost)
    {
        Shutdown();
        mState = AppState::Failed;
        return mState;
    }
    if (startup.State != rhi::StartupState::Ready)
    {
        return mState;
    }
    if (PrepareRenderer() == false)
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
        BrowserInput(window);
    }
    else
    {
        const auto native = mWindow->GetNativeWindowInfo();
        // Like a hidden browser page, a hidden native window drops simulation time instead of ticking.
        window.Visible = isWindowVisible;
        window.FramebufferWidth = native.Width;
        window.FramebufferHeight = native.Height;
    }
    if (mRendererReady == false || mSession.World().IsBuilt() == false)
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
    if (Render(window) == false)
    {
        Shutdown();
        mState = AppState::Failed;
    }
    LUDUS_PROFILE_FRAME();
    return mState;
}
} // namespace ludus::world_demo
