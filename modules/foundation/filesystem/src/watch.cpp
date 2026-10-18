#include <ludus/foundation/filesystem/watch.hpp>

#if defined(LUDUS_FILESYSTEM_WATCH_FAULT_TESTING)
#    include "internal/watch_test_hooks.hpp"
#endif

#include <atomic>
#include <new>
#include <string_view>

// Thanks to Noel Llopis and Charles Nicholson, "Stay in the Game: Asset
// Hotloading for Fast Iteration", Game Programming Gems 6, sec. 1.10,
// pp. 109-114: the existing filesystem design adopts their prepare/rebind
// boundary. This bounded adapter emits hints; Content owns validation,
// rebinding and resource retirement. No article code copied. See
// docs/architecture/filesystem.md for the reference review and F5 limits.

namespace ludus::foundation::filesystem
{
namespace
{
template <typename T>
T* Allocate(usize count) noexcept
{
#if defined(LUDUS_FILESYSTEM_WATCH_FAULT_TESTING)
    if (watch_test::AllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T[count]();
}
std::atomic<uint64> gOwner{1};
bool Identity(uint64& output) noexcept
{
    uint64 value = gOwner.load(std::memory_order_relaxed);
    do
    {
        if (value == ~uint64{0})
        {
            return false;
        }
    } while (gOwner.compare_exchange_weak(value, value + 1, std::memory_order_relaxed) == false);
    output = value;
    return true;
}
bool Known(Result observation) noexcept
{
    return observation.Succeeded() || observation.Code == Status::NotFound;
}
bool Same(Result first, const FileStamp& a, Result second, const FileStamp& b) noexcept
{
    return first.Code == second.Code && (first.Code == Status::NotFound || a == b);
}
struct Slot final
{
    WatchHint Baseline;
    Result CandidateResult;
    FileStamp Candidate;
    uint64 Since = 0;
    bool Active = false;
    bool Pending = false;
};
} // namespace
struct Watcher::Impl final
{
    Directory Root;
    WatchConfig Config;
    Slot* Slots = nullptr;
    WatchHint* Hints = nullptr;
    uint64 Owner = 0;
    uint64 Sequence = 1;
    uint64 LastTime = 0;
    uint64 Dropped = 0;
    usize Cursor = 0;
    usize Head = 0;
    usize Count = 0;
    usize Scan = 0;
    bool Rescan = false;
    bool Notice = false;
    bool Scanning = false;
    bool ScanFailed = false;
    ~Impl() noexcept
    {
        delete[] Slots;
        delete[] Hints;
    }
    void Invalidate(uint64 additional) noexcept
    {
        const uint64 lost = static_cast<uint64>(Count) + additional;
        Dropped = lost > ~uint64{0} - Dropped ? ~uint64{0} : Dropped + lost;
        Head = Count = 0;
        Rescan = Notice = true;
    }
};
std::string_view WatchHint::PathView() const noexcept
{
    return {Path, PathBytes};
}
Watcher::~Watcher() noexcept
{
    delete[] mImpl;
}
Result Watcher::Init(std::string_view nativeRoot, const WatchConfig& config) noexcept
{
    if (config.MaxPaths == 0 || config.MaxPaths > 4096 || config.HintCapacity == 0 || config.HintCapacity > 4096)
    {
        return {Status::InvalidArgument};
    }
    // Detect unsupported native roots before allocating registration storage.
    Directory root;
    const auto opened = root.Open(nativeRoot);
    if (opened.Succeeded() == false)
    {
        return opened;
    }
    auto* next = Allocate<Impl>(1);
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    next->Config = config;
    next->Root = Move(root);
    next->Slots = Allocate<Slot>(config.MaxPaths);
    next->Hints = Allocate<WatchHint>(config.HintCapacity);
    if (next->Slots == nullptr || next->Hints == nullptr)
    {
        delete[] next;
        return {Status::OutOfMemory};
    }
    if (Identity(next->Owner) == false)
    {
        delete[] next;
        return {Status::LimitExceeded};
    }
    delete[] mImpl;
    mImpl = next;
    return {};
}
Result Watcher::Add(std::string_view path, uint64 tag, WatchHandle& output) noexcept
{
    if (mImpl == nullptr || ValidPath(path) == false)
    {
        return {Status::InvalidArgument};
    }
    auto& state = *mImpl;
    if (state.Scanning)
    {
        return {Status::Conflict};
    }
    usize available = state.Config.MaxPaths;
    for (usize i = 0; i < state.Config.MaxPaths; ++i)
    {
        const auto& slot = state.Slots[i];
        if (slot.Active && slot.Baseline.PathView() == path)
        {
            return {Status::InvalidArgument};
        }
        if (slot.Active == false && available == state.Config.MaxPaths)
        {
            available = i;
        }
    }
    if (available == state.Config.MaxPaths || state.Sequence == ~uint64{0})
    {
        return {Status::LimitExceeded};
    }
    FileStamp stamp;
    const auto observation = state.Root.Observe(path, stamp);
    if (Known(observation) == false)
    {
        return observation;
    }
    auto& slot = state.Slots[available];
    slot = {};
    slot.Active = true;
    slot.Baseline.Handle = {state.Owner, state.Sequence++, available};
    slot.Baseline.Tag = tag;
    slot.Baseline.Observation = observation;
    slot.Baseline.Stamp = stamp;
    slot.Baseline.PathBytes = path.size();
    for (usize i = 0; i < path.size(); ++i)
    {
        slot.Baseline.Path[i] = path[i];
    }
    output = slot.Baseline.Handle;
    return {};
}
Result Watcher::Remove(WatchHandle handle) noexcept
{
    if (mImpl == nullptr || handle.Owner != mImpl->Owner || handle.Slot >= mImpl->Config.MaxPaths ||
        mImpl->Slots[handle.Slot].Active == false || mImpl->Slots[handle.Slot].Baseline.Handle != handle)
    {
        return {Status::InvalidArgument};
    }
    auto& state = *mImpl;
    if (state.Scanning)
    {
        return {Status::Conflict};
    }
    state.Slots[handle.Slot] = {};
    usize kept = 0;
    for (usize i = 0; i < state.Count; ++i)
    {
        const usize source = (state.Head + i) % state.Config.HintCapacity;
        if (state.Hints[source].Handle != handle)
        {
            state.Hints[(state.Head + kept++) % state.Config.HintCapacity] = state.Hints[source];
        }
    }
    state.Count = kept;
    return {};
}
Result Watcher::Advance(uint64 nowNanoseconds, usize maxSteps) noexcept
{
    if (mImpl == nullptr || maxSteps == 0 || maxSteps > mImpl->Config.MaxPaths || nowNanoseconds < mImpl->LastTime)
    {
        return {Status::InvalidArgument};
    }
    auto& state = *mImpl;
    if (state.Scanning || state.Rescan)
    {
        return {Status::Conflict};
    }
    state.LastTime = nowNanoseconds;
    for (usize step = 0; step < maxSteps; ++step)
    {
        auto& slot = state.Slots[state.Cursor];
        state.Cursor = (state.Cursor + 1) % state.Config.MaxPaths;
        if (slot.Active == false)
        {
            continue;
        }
        FileStamp stamp;
        const auto observation = state.Root.Observe(slot.Baseline.PathView(), stamp);
        if (Known(observation) == false)
        {
            state.Invalidate(0);
            return observation;
        }
        if (Same(observation, stamp, slot.Baseline.Observation, slot.Baseline.Stamp))
        {
            slot.Pending = false;
            continue;
        }
        if (slot.Pending == false || Same(observation, stamp, slot.CandidateResult, slot.Candidate) == false)
        {
            slot.Pending = true;
            slot.CandidateResult = observation;
            slot.Candidate = stamp;
            slot.Since = nowNanoseconds;
        }
        if (nowNanoseconds - slot.Since < state.Config.DebounceNanoseconds)
        {
            continue;
        }
        if (state.Count == state.Config.HintCapacity)
        {
            state.Invalidate(1);
            return {Status::LimitExceeded};
        }
        slot.Baseline.Observation = observation;
        slot.Baseline.Stamp = stamp;
        slot.Pending = false;
        state.Hints[(state.Head + state.Count++) % state.Config.HintCapacity] = slot.Baseline;
    }
    return {};
}
WatchPollResult Watcher::Poll(WatchHint& output) noexcept
{
    if (mImpl == nullptr)
    {
        return {{Status::InvalidArgument}};
    }
    auto& state = *mImpl;
    if (state.Scanning)
    {
        return {{Status::Conflict}};
    }
    if (state.Notice)
    {
        output = {};
        output.Kind = WatchHintKind::RescanRequired;
        state.Notice = false;
        return {{}, true};
    }
    if (state.Count == 0)
    {
        return {};
    }
    output = state.Hints[state.Head];
    state.Head = (state.Head + 1) % state.Config.HintCapacity;
    --state.Count;
    return {{}, true};
}
Result Watcher::BeginRescan() noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    auto& state = *mImpl;
    if (state.Scanning)
    {
        return {Status::Conflict};
    }
    state.Head = state.Count = state.Scan = 0;
    state.Scanning = true;
    state.Rescan = true;
    state.Notice = false;
    state.ScanFailed = false;
    return {};
}
WatchPollResult Watcher::NextRescan(WatchHint& output) noexcept
{
    if (mImpl == nullptr)
    {
        return {{Status::InvalidArgument}};
    }
    auto& state = *mImpl;
    if (state.Scanning == false)
    {
        return {{Status::Conflict}};
    }
    while (state.Scan < state.Config.MaxPaths)
    {
        auto& slot = state.Slots[state.Scan++];
        if (slot.Active == false)
        {
            continue;
        }
        output = slot.Baseline;
        output.Kind = WatchHintKind::RescanEntry;
        output.Stamp = {};
        output.Observation = state.Root.Observe(output.PathView(), output.Stamp);
        if (Known(output.Observation))
        {
            slot.Baseline.Observation = output.Observation;
            slot.Baseline.Stamp = output.Stamp;
            slot.Pending = false;
        }
        else
        {
            state.ScanFailed = true;
        }
        return {{}, true};
    }
    state.Scanning = false;
    state.Rescan = state.Notice = state.ScanFailed;
    return {};
}
bool Watcher::NeedsRescan() const noexcept
{
    return mImpl != nullptr && mImpl->Rescan;
}
bool Watcher::IsRescanning() const noexcept
{
    return mImpl != nullptr && mImpl->Scanning;
}
uint64 Watcher::DroppedHints() const noexcept
{
    return mImpl != nullptr ? mImpl->Dropped : 0;
}
} // namespace ludus::foundation::filesystem
