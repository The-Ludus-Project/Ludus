#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/browser/window.h>

#include <emscripten.h>

namespace
{
namespace foundation = ludus::foundation;
namespace platform = ludus::platform;
namespace rhi = ludus::graphics::rhi;

platform::WindowManager gWindowManager;
foundation::UniquePtr<platform::Window> gWindow;
struct GameState final
{
    bool IsPlaying = false;
    foundation::uint32 Score = 0;
    foundation::uint32 DisplayedScore = 0;
};
GameState gGame;

constexpr foundation::float64 READY_GREEN = 0.4;
constexpr foundation::float64 SCORE_BLUE = 0.6;

// clang-format off
EM_JS(void, ShowStatus, (int isPlaying, int error, foundation::uint32 score), {
    document.getElementById('status').textContent = error
        ? 'WebGPU failed (' + error + '). Use a desktop browser and device with WebGPU support, then reload.'
        : isPlaying ? 'Score: ' + score : 'Loading WebGPU…';
});
// clang-format on

void DrawFrame() noexcept
{
    (void)gWindow->HandleEvent({});
    const rhi::StartupInfo startup = rhi::GetStartup();
    if (startup.State == rhi::StartupState::Failed || startup.State == rhi::StartupState::DeviceLost)
    {
        ShowStatus(0, static_cast<int>(startup.Error), gGame.Score);
        emscripten_cancel_main_loop();
        return;
    }
    if (startup.State != rhi::StartupState::Ready)
    {
        return;
    }

    platform::browser::WindowState window;
    if (gWindow->GetBrowserState(window) == false)
    {
        return;
    }
    platform::browser::InputEvent event;
    while (gWindow->PollBrowserInput(event) == true)
    {
        if (event.Kind == platform::browser::EventKind::KeyDown && event.PhysicalKey == platform::browser::Key::Space &&
            event.Repeat == false && gGame.IsPlaying == true && window.Focused == true)
        {
            ++gGame.Score;
        }
    }
    const bool isEven = gGame.Score % 2 == 0;
    const rhi::FrameTarget target =
    {
        .Width = window.FramebufferWidth,
        .Height = window.FramebufferHeight,
        .Green = isEven == true ? READY_GREEN : 0,
        .Blue = isEven == true ? 0 : SCORE_BLUE,
    };
    if (rhi::SetFrameTarget(target) != rhi::FrameStatus::Ready || rhi::BeginFrameStatus() != rhi::FrameStatus::Ready ||
        rhi::EndFrameStatus() != rhi::FrameStatus::Ready)
    {
        return;
    }
    if (gGame.IsPlaying == false || gGame.Score != gGame.DisplayedScore)
    {
        gGame.IsPlaying = true;
        gGame.DisplayedScore = gGame.Score;
        ShowStatus(1, 0, gGame.Score);
    }
}
} // namespace

int main()
{
    if (gWindowManager.Initialize({}) == false || gWindowManager.CreateWindow({}, gWindow) == false)
    {
        ShowStatus(0, static_cast<int>(rhi::StartupError::InvalidWindow), 0);
        return 1;
    }
    if (rhi::Start({ .Name = "R5 Color Rally" }, gWindow->GetNativeWindowInfo(), rhi::BackendSelection::WebGPU) ==
        rhi::StartStatus::Failed)
    {
        ShowStatus(0, static_cast<int>(rhi::GetStartup().Error), 0);
        return 1;
    }
    emscripten_set_main_loop(DrawFrame, 0, false);
    return 0;
}
