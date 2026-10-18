#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_api/services.h>

#include <emscripten.h>

// Static browser player for the same sample GameApi entry used by native shipping
// builds. The page owns input/focus; the player owns instance and RHI lifetimes.
extern "C" ludus::runtime::game_api::Status
LudusGetGameApi(ludus::foundation::uint32, ludus::foundation::uint32, ludus::runtime::game_api::GameApiTable*) noexcept;

namespace
{
using namespace ludus::foundation;
namespace game = ludus::runtime::game_api;
namespace rhi = ludus::graphics::rhi;
core::UniquePtr<ludus::platform::Window> gWindow;
game::GameApiTable gApi;
game::GameInstance* gInstance = nullptr;
game::RenderParams gRender;
uint64 gFrames = 0;
float64 gPrevious = 0;
float64 gElapsed = 0;
bool gPaused = false;
bool gFailed = false;

// clang-format off
EM_JS(void, Present, (const char* state, uint32 frames), {
    globalThis.ludusPlayerStatus(UTF8ToString(state), frames);
});
EM_JS(uint32, HeldActions, (), {
    const actions = (Module.heldActions || 0) | (Module.pendingActions || 0);
    Module.pendingActions = 0;
    return actions;
});
// clang-format on

void Fail() noexcept
{
    if (gInstance != nullptr)
    {
        gApi.Destroy(gInstance);
        gInstance = nullptr;
    }
    rhi::Shutdown();
    gWindow.Reset();
    gFailed = true;
    Present("failed", static_cast<uint32>(gFrames));
}
float64 Color(float32 value) noexcept
{
    return value >= 0.0F ? (value > 1.0F ? 1.0 : static_cast<float64>(value)) : 0.0;
}
void Frame() noexcept
{
    if (gFailed)
    {
        return;
    }
    const auto startup = rhi::GetStartup().State;
    if (startup == rhi::StartupState::Pending)
    {
        return;
    }
    if (startup != rhi::StartupState::Ready || gWindow->HandleEvent({}) == false)
    {
        Fail();
        return;
    }
    const auto window = gWindow->GetNativeWindowInfo();
    const float64 now = emscripten_get_now() / 1000.0;
    const float64 delta = gPrevious == 0.0 || gPaused ? 0.0 : (now - gPrevious > 0.1 ? 0.1 : now - gPrevious);
    gPrevious = now;
    gElapsed += delta;
    const game::FrameInput input
    {
        .FrameIndex = gFrames,
        .DeltaSeconds = delta,
        .ElapsedSeconds = gElapsed,
        .Width = window.Width,
        .Height = window.Height,
        .HeldActions = HeldActions(),
        .Paused = static_cast<uint8>(gPaused),
    };
    if (gPaused == false && gApi.Update(gInstance, &input, &gRender) != game::Status::Ok)
    {
        Fail();
        return;
    }
    const auto target = rhi::SetFrameTarget(
    {
        .Width = window.Width,
        .Height = window.Height,
        .Red = Color(gRender.ClearRed),
        .Green = Color(gRender.ClearGreen),
        .Blue = Color(gRender.ClearBlue),
        .Alpha = Color(gRender.ClearAlpha),
    });
    if (target == rhi::FrameStatus::Skipped)
    {
        return;
    }
    const auto begun = target == rhi::FrameStatus::Ready ? rhi::BeginFrameStatus() : target;
    if (begun == rhi::FrameStatus::Skipped)
    {
        return;
    }
    if (begun != rhi::FrameStatus::Ready || rhi::EndFrameStatus() != rhi::FrameStatus::Ready)
    {
        Fail();
        return;
    }
    ++gFrames;
    Present(gPaused ? "paused" : "playing", static_cast<uint32>(gFrames));
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void SetPaused(ludus::foundation::uint32 paused) noexcept
{
    gPaused = paused != 0;
}

int main()
{
    using namespace ludus::foundation;
    gApi.StructSize = sizeof(gApi);
    const game::CreateInfo info
    {
        .StructSize = sizeof(game::CreateInfo),
        .ProjectId = 1,
        .GameId = 1,
        .ModuleGeneration = 1,
        .AuthoredDocument = {},
    };
    ludus::platform::WindowManager manager;
    if (LudusGetGameApi(game::kAbiMajor, game::kAbiMinor, &gApi) != game::Status::Ok ||
        gApi.Create(&info, &gInstance) != game::Status::Ok || manager.Initialize({}) == false ||
        manager.CreateWindow({ .Name = "Ludus web sample", .Width = 800, .Height = 600 }, gWindow) == false)
    {
        Fail();
        return 1;
    }
    const auto started = rhi::Start({ .Name = "Ludus web sample", .Version = 1 }, gWindow->GetNativeWindowInfo());
    if (started != rhi::StartStatus::Ready && started != rhi::StartStatus::Pending)
    {
        Fail();
        return 1;
    }
    emscripten_set_main_loop(Frame, 0, true);
}
