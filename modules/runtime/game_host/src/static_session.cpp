#include "internal/frame_presenter.h"
#include "internal/host_pacing.h"
#include "internal/host_services.h"
#include "internal/identity.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/platform/frame_pacing.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_host/host.h>

namespace ludus::runtime::game_host
{
namespace
{
constexpr foundation::float64 TICKS_PER_SECOND = 60.0;
constexpr uint32 HEADLESS_WIDTH = 800;
constexpr uint32 HEADLESS_HEIGHT = 600;
} // namespace

using game_api::CreateInfo;
using game_api::FrameInput;
using game_api::GameInstance;
using game_api::RenderParams;
using game_api::Status;

RunResult RunStatic(const HostConfig& config, StaticEntryFn entryOrNull) noexcept
{
    if (entryOrNull == nullptr)
    {
        return RunResult::BadArguments;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    if (config.ControlFd >= 0 || config.AwaitLoad == true)
    {
        return RunResult::ProtocolError;
    }
#endif
    // Fill the table in-process (static dispatch replaces only the module
    // lookup; design 2). No identity check is needed — the implementation is
    // compiled into this binary against this SDK.
    game_api::GameApiTable table = {};
    table.StructSize = static_cast<uint32>(sizeof(game_api::GameApiTable));
    table.AbiMajor = CurrentAbiMajor();
    table.AbiMinor = CurrentAbiMinor();
    const Status fillStatus = entryOrNull(CurrentAbiMajor(), CurrentAbiMinor(), &table);
    if (fillStatus != Status::Ok || table.Create == nullptr || table.Update == nullptr || table.Destroy == nullptr)
    {
        return RunResult::IncompatibleModule;
    }

    HostServiceProvider services;
    CreateInfo info = {};
    info.StructSize = static_cast<uint32>(sizeof(CreateInfo));
    info.Services = &services.Services();
    info.ProjectId = config.ProjectId;
    info.GameId = config.GameId;
    info.ModuleGeneration = 1;
    info.AuthoredDocument = config.AuthoredDocument;
    GameInstance* instanceOrNull = nullptr;
    if (table.Create(&info, &instanceOrNull) != Status::Ok || instanceOrNull == nullptr)
    {
        services.Retire();
        if (instanceOrNull != nullptr)
        {
            table.Destroy(instanceOrNull);
        }
        return RunResult::Internal;
    }
    services.Activate();

    FramePresenter presenter;
    const RunResult started = presenter.Start(config.Mode);
    if (started != RunResult::Ok)
    {
        services.Retire();
        table.Destroy(instanceOrNull);
        return started;
    }
    RunResult result = RunResult::Ok;
    HostPacing pacing;
    const platform::PacingClock pacingClock = platform::GetSystemPacingClock();
    for (uint64 i = 0; config.MaxFrames == 0 || i < config.MaxFrames; ++i)
    {
        // The same absolute schedule and hidden rate as the play session, without telemetry.
        (void)pacing.Pace(pacingClock);
        if (presenter.Poll() == false)
        {
            break;
        }
        pacing.SetVisible(presenter.IsWindowVisible());
        FrameInput input = {};
        input.FrameIndex = i;
        input.DeltaSeconds = 1.0 / TICKS_PER_SECOND;
        input.ElapsedSeconds = static_cast<foundation::float64>(i) / TICKS_PER_SECOND;
        input.Width = HEADLESS_WIDTH;
        input.Height = HEADLESS_HEIGHT;
        presenter.FillInput(input);
        RenderParams render = {};
        if (table.Update(instanceOrNull, &input, &render) != Status::Ok)
        {
            result = RunResult::Internal;
            break;
        }
        result = presenter.Present(render);
        if (result != RunResult::Ok)
        {
            break;
        }
    }
    services.GateWork();
    if ((table.Quiesce != nullptr && table.Quiesce(instanceOrNull) != Status::Ok) || services.OutstandingWork() != 0)
    {
        services.PinUntilProcessExit();
        return RunResult::Internal;
    }
    services.Retire();
    table.Destroy(instanceOrNull);
    return services.OutstandingAllocations() == 0 && services.OutstandingWork() == 0 ? result : RunResult::Internal;
}

} // namespace ludus::runtime::game_host
