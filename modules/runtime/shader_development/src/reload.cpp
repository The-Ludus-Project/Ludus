#include <ludus/runtime/shader_development/reload.hpp>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include <new>
#include <string_view>

namespace ludus::runtime::shader_development
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace rhi = graphics::rhi;
namespace filesystem = foundation::filesystem;
namespace format = content::render_graph_format;
struct ReloadSession::State final
{
    renderer::RenderGraphActivation* ActivationOrNull = nullptr;
    filesystem::MountSnapshot Mounts;
    filesystem::VirtualPath Path;
    filesystem::Watcher Watcher;
    Array<uint8> Bytes;
    filesystem::AsyncReader Reader; // Destroy/drain before releasing Bytes.
    filesystem::RequestHandle Read;
    ReloadReport Report;
    bool HasRead = false;
    bool ShouldReload = true;
};
ReloadSession::ReloadSession() noexcept = default;
ReloadSession::~ReloadSession() noexcept = default;
rhi::RasterStatus ReloadSession::Initialize(BorrowedView<const char> root,
                                            BorrowedView<const char> path,
                                            renderer::RenderGraphActivation& inoutActivation) noexcept
{
    if (mState.Get() != nullptr)
    {
        return rhi::RasterStatus::InvalidState;
    }
    if (root.IsValid() == false || path.IsValid() == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> state{allocationOrNull};
    if (state.Get() == nullptr)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    const std::string_view rootText{root.GetDataOrNull(), root.GetSize()};
    const std::string_view pathText{path.GetDataOrNull(), path.GetSize()};
    filesystem::ProviderHandle provider;
    filesystem::WatchConfig watchConfig;
    watchConfig.MaxPaths = 1;
    watchConfig.HintCapacity = 1;
    watchConfig.DebounceNanoseconds = 0; // Producer uses atomic publication.
    filesystem::WatchHandle registration;
    if (filesystem::CreateDirectoryProvider(rootText, provider).Succeeded() == false ||
        state->Path.Set(filesystem::Root::Cache, pathText).Succeeded() == false ||
        state->Watcher.Init(rootText, watchConfig).Succeeded() == false ||
        state->Watcher.Add(pathText, 0, registration).Succeeded() == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    filesystem::Mount mount;
    mount.Namespace = filesystem::Root::Cache;
    mount.Id = 1;
    mount.Source = Move(provider);
    if (filesystem::MountSnapshot::Create({&mount, 1}, 1, state->Mounts).Succeeded() == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    filesystem::AsyncConfig readConfig;
    readConfig.WorkerCount = 1;
    readConfig.MaxRequests = 1;
    readConfig.MaxBytes = format::MAX_PACKAGE_BYTES;
    if (state->Reader.Initialize(readConfig) != filesystem::AsyncStatus::Ok)
    {
        return rhi::RasterStatus::Unsupported;
    }
    state->ActivationOrNull = &inoutActivation;
    mState = Move(state);
    return rhi::RasterStatus::Ready;
}
void ReloadSession::RequestReload() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->ShouldReload = true;
    }
}
ReloadReport ReloadSession::Service(uint64 nowNanoseconds) noexcept
{
    if (mState.Get() == nullptr)
    {
        return {ReloadFailure::StageRejected, ReloadState::Failed, 0, rhi::RasterStatus::InvalidState};
    }
    State& state = *mState;
    if (state.Watcher.NeedsRescan() == true)
    {
        if (state.Watcher.IsRescanning() == false)
        {
            static_cast<void>(state.Watcher.BeginRescan());
        }
        filesystem::WatchHint hint;
        const filesystem::WatchPollResult observed = state.Watcher.NextRescan(hint);
        if (observed.Available == true)
        {
            state.ShouldReload = true;
        }
    }
    else
    {
        if (state.Watcher.Advance(nowNanoseconds, 1).Succeeded() == false)
        {
            state.Report.State = ReloadState::Failed;
        }
        filesystem::WatchHint hint;
        if (state.Watcher.Poll(hint).Available == true)
        {
            state.ShouldReload = true;
        }
    }
    if (state.HasRead == true)
    {
        if (state.ShouldReload == true)
        {
            static_cast<void>(state.Reader.Cancel(state.Read));
        }
        filesystem::ReadCompletion completed;
        if (state.Reader.Poll(completed) == false)
        {
            return state.Report;
        }
        state.HasRead = false;
        if (state.ShouldReload == false)
        {
            state.Report.State = ReloadState::Failed;
            state.Report.Failure = ReloadFailure::Unreadable;
            if (completed.Disposition == filesystem::ReadDisposition::Read &&
                completed.Read.Outcome.Succeeded() == true && completed.Read.BytesRead == state.Bytes.GetSize())
            {
                renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
                core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
                state.Report.Failure = package.Get() == nullptr ? ReloadFailure::OutOfMemory : ReloadFailure::Malformed;
                if (package.Get() != nullptr && package->Decode({state.Bytes.GetData(), state.Bytes.GetSize()}) ==
                                                    renderer::RenderGraphPackageStatus::Ready)
                {
                    state.Report.LastStage = state.ActivationOrNull->Stage(Move(package));
                    state.Report.Failure = (state.Report.LastStage == rhi::RasterStatus::Ready ||
                                            state.Report.LastStage == rhi::RasterStatus::Pending)
                                               ? ReloadFailure::None
                                               : ReloadFailure::StageRejected;
                    const renderer::RenderGraphCandidateState candidate = state.ActivationOrNull->GetReport().Candidate;
                    state.Report.State = candidate == renderer::RenderGraphCandidateState::RestartRequired
                                             ? ReloadState::RestartRequired
                                         : (state.Report.LastStage == rhi::RasterStatus::Ready ||
                                            state.Report.LastStage == rhi::RasterStatus::Pending)
                                             ? ReloadState::Staged
                                             : ReloadState::Failed;
                }
            }
        }
    }
    if (state.ShouldReload == true && state.HasRead == false)
    {
        state.ShouldReload = false;
        state.Report.State = ReloadState::Failed;
        if (state.Report.Request == static_cast<uint64>(-1))
        {
            return state.Report;
        }
        ++state.Report.Request;
        filesystem::VirtualFile file;
        state.Report.Failure = ReloadFailure::Unreadable;
        if (state.Mounts.OpenRead(state.Path, file).Outcome.Succeeded() == false)
        {
            return state.Report;
        }
        state.Report.Failure = ReloadFailure::TooLarge;
        if (file.Size() == 0 || file.Size() > format::MAX_PACKAGE_BYTES)
        {
            return state.Report;
        }
        state.Report.Failure = ReloadFailure::OutOfMemory;
        if (state.Bytes.TryResize(static_cast<usize>(file.Size())) == false)
        {
            return state.Report;
        }
        filesystem::ReadOptions options;
        options.Tag = state.Report.Request;
        if (state.Reader.Submit(file, 0, state.Bytes.AsSpan(), state.Read, options) == filesystem::AsyncStatus::Ok)
        {
            state.Report.Failure = ReloadFailure::None;
            state.HasRead = true;
            state.Report.State = ReloadState::Reading;
        }
    }
    return state.Report;
}
} // namespace ludus::runtime::shader_development
