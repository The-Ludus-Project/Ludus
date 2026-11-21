#pragma once
#include "frame_pacing.h"
#include "frame_renderer.h"
#include "world.h"
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/hosted_recorder.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/input/keyboard.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/browser/window.h>
namespace ludus::world_demo
{
enum class AppState : uint8
{
    Stopped,
    Loading,
    Playing,
    Failed
};
/// The World-demo player: one window, one explicit RHI device and the engine Renderer.
///
/// In a hosted window (`LUDUS_HOSTED_WINDOW`, Play in the editor's World viewport) the
/// device is created with GPU timing and a HostedGpuRecorder answers the editor's GPU
/// recordings with the Renderer's named viewport passes. The game never arms pass timing
/// itself; the recorder owns it.
class Application final
{
public:
    [[nodiscard]] bool Start(graphics::rhi::BackendSelection selection = graphics::rhi::BackendSelection::Auto,
                             const char* levelBytes = nullptr,
                             usize levelSize = 0) noexcept;
    [[nodiscard]] AppState Frame() noexcept;
    void Shutdown() noexcept;
    /// Opt in to immediate planar player follow; default retains authored fixed camera.
    void SetCameraFollow(bool enabled) noexcept
    {
        mCameraFollow = enabled;
    }
    [[nodiscard]] const Session& GetSession() const noexcept
    {
        return mSession;
    }
    [[nodiscard]] uint64 GetPresentedFrames() const noexcept
    {
        return mPresentedFrames;
    }
    [[nodiscard]] AppState GetState() const noexcept
    {
        return mState;
    }
    /// Select the native frame-rate cap; zero keeps swapchain pacing with idle waits (see LoopPacing).
    /// @return false for an out-of-range cap, preserving the previous one.
    [[nodiscard]] bool SetFrameRateLimit(uint32 framesPerSecond) noexcept
    {
        return mPacing.Configure(framesPerSecond);
    }
    /// Pace the native outer loop after Frame(). Browser builds, stopped and failed applications
    /// return Disabled without waiting: browsers pace with presentation callbacks.
    [[nodiscard]] platform::PaceResult Pace(const platform::PacingClock& clock) noexcept;
    /// Reading taken when the latest presented frame's EndFrame returned (CPU side, not scanout).
    [[nodiscard]] foundation::time::Timestamp GetLastPresentReturn() const noexcept
    {
        return mPresentReturn;
    }
    /// First activation of a built World, on the same clock as presentation.
    [[nodiscard]] LUDUS_INLINE foundation::time::Timestamp GetFirstWorldActivation() const noexcept
    {
        return mFirstWorldActivation;
    }
    /// First successful renderer preparation on this run.
    [[nodiscard]] LUDUS_INLINE foundation::time::Timestamp GetFirstRendererReady() const noexcept
    {
        return mFirstRendererReady;
    }
    /// Initial Session load request, including World text decode and validation.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetInitialLoadRequestDuration() const noexcept
    {
        return mInitialLoadRequestDuration;
    }
    /// Unit-quad upload submission through the renderer's first Ready poll.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetQuadReadyDuration() const noexcept
    {
        return mFrameRenderer.GetQuadReadyDuration();
    }
    /// Backend chosen by the device; meaningful once the device has reported Ready.
    [[nodiscard]] graphics::rhi::Backend GetSelectedBackend() const noexcept
    {
        return mSelectedBackend;
    }

private:
    [[nodiscard]] AppState playFrame(const graphics::rhi::StartupInfo& startup,
                                     bool isWindowVisible,
                                     foundation::time::Timestamp now) noexcept;
    [[nodiscard]] bool render(const platform::browser::WindowState& window) noexcept;
    void browserInput(const platform::browser::WindowState& window) noexcept;
    /// Admit a game flow request; a Load restarts the example room.
    void requestFlow(FlowRequestKind kind) noexcept;
    /// Run a flow boundary and apply its presentation consequences.
    void processFlowBoundary() noexcept;
    input::InputSystem mInput;
    foundation::UniquePtr<platform::Window> mWindow;
    Session mSession;
    RenderFrame mFrame;
    graphics::rhi::DeviceHandle mDevice;
    graphics::rhi::SurfaceHandle mSurface;
    graphics::rhi::Backend mSelectedBackend = graphics::rhi::Backend::Vulkan;
    FrameRenderer mFrameRenderer;
    /// Attached only in a hosted window; a detached recorder ignores every frame.
    graphics::rhi::HostedGpuRecorder mRecorder;
    bool mIsRendererReady = false;
    LoopPacing mPacing;
    foundation::time::Timestamp mPresentReturn;
    foundation::time::Timestamp mFirstWorldActivation;
    foundation::time::Timestamp mFirstRendererReady;
    foundation::time::Duration mInitialLoadRequestDuration;
    uint64 mPresentedFrames = 0;
    uint64 mPacedPresentedFrames = 0;
    uint64 mInputFrame = 0;
    bool mHasDevice = false;
    bool mHadFocus = false;
    bool mCameraFollow = false;
    AppState mState = AppState::Stopped;
};
} // namespace ludus::world_demo
