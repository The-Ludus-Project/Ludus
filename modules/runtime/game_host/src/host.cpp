#include <ludus/runtime/game_host/host.h>

#include <ludus/foundation/base/config.h>

#if LUDUS_TARGET_OS != LUDUS_OS_WINDOWS
#    include "internal/frame_presenter.h"
#    include "internal/host_session.h"
#endif
#include "internal/identity.h"

namespace ludus::runtime::game_host
{
std::string_view HostIdentity() noexcept
{
    return CurrentHostIdentity();
}

uint32 HostAbiMajor() noexcept
{
    return CurrentAbiMajor();
}

uint32 HostAbiMinor() noexcept
{
    return CurrentAbiMinor();
}

RunResult InspectModule(std::string_view path, game_api::GameMetadata& metadata) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    (void)path;
    (void)metadata;
    return RunResult::ModuleLoadFailed;
#else
    LoadedModule module;
    const auto loaded = LoadModule(path, 1, CurrentHostIdentity(), CurrentAbiMajor(), CurrentAbiMinor(), module);
    if (loaded != LoadStatus::Ok)
    {
        return loaded == LoadStatus::DlopenFailed || loaded == LoadStatus::PathInvalid ? RunResult::ModuleLoadFailed
                                                                                       : RunResult::IncompatibleModule;
    }
    metadata = module.Metadata();
    return module.Close() ? RunResult::Ok : RunResult::Internal;
#endif
}

RunResult Run(const HostConfig& config) noexcept
{
    if (config.Source != GameplaySource::DynamicModule)
    {
        // Static dispatch is wired by the shipping executable (handled there).
        return RunResult::BadArguments;
    }
    if (config.ModulePath.empty())
    {
        return RunResult::BadArguments;
    }

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    return RunResult::ModuleLoadFailed;
#else
    FramePresenter presenter;
    const auto started = presenter.Start(config.Mode);
    if (started != RunResult::Ok)
    {
        return started;
    }
    HostSession session(config.ProjectId, config.GameId, config.ControlFd, config.ProjectEpoch);
    const bool loaded =
        config.AwaitLoad
            ? session.PrepareInitialLoad(config.ModulePath, config.InitialGeneration, config.AuthoredDocument)
            : session.LoadInitial(config.ModulePath, config.InitialGeneration, config.AuthoredDocument);
    if (loaded == false)
    {
        return session.InitialFailure();
    }
    return session.RunLoop(config.MaxFrames, &presenter);
#endif
}
} // namespace ludus::runtime::game_host
