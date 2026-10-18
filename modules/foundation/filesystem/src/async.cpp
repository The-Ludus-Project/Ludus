#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>

#include "internal/async_native.hpp"
#if defined(LUDUS_FILESYSTEM_ASYNC_FAULT_TESTING)
#    include "internal/async_test_hooks.hpp"
#endif

#include <atomic>
#include <chrono>
#include <new>
#include <span>

namespace ludus::foundation::filesystem
{
namespace
{
constexpr uint64 MAX_COUNTER = ~uint64{0};
thread_local const void* gReadingScheduler = nullptr;
#if !defined(LUDUS_PLATFORM_WEB)
std::atomic<uint64> gNextScheduler{1};
#endif
void Add(uint64& value, uint64 amount = 1) noexcept
{
    value = amount > MAX_COUNTER - value ? MAX_COUNTER : value + amount;
}
#if defined(LUDUS_FILESYSTEM_ASYNC_FAULT_TESTING)
#    define LUDUS_ASYNC_FAIL(point) detail::FailAsync(detail::AsyncFault::point)
#else
#    define LUDUS_ASYNC_FAIL(point) false
#endif
} // namespace

uint64 AsyncNow() noexcept
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct AsyncReader::Impl
{
    struct Slot
    {
        VirtualFile File;
        std::span<uint8> Destination;
        ReadCompletion Completion;
        uint64 Offset = 0;
        uint64 Deadline = 0;
        uint64 AcceptedAt = 0;
        uint64 StartedAt = 0;
        uint64 CancelledAt = 0;
        uint64 TerminalOrder = 0;
        int32 Priority = 0;
        RequestState State = RequestState::Completed;
        bool Occupied = false;
        bool Cancellation = false;
    };
    detail::IoGate Gate;
    AsyncConfig Config;
    Slot* Slots = nullptr;
    detail::IoWorker* Workers = nullptr;
    ReadTrace* Traces = nullptr;
    VirtualPath* Paths = nullptr;
    AsyncMetrics Counters;
    uint64 Identity = 0;
    uint64 NextSequence = 1;
    uint64 NextTerminal = 1;
    uint32 StartedWorkers = 0;
    uint32 TraceHead = 0;
    uint32 TraceCount = 0;
    bool Stopping = false;
    bool GateReady = false;

    ~Impl() noexcept
    {
        delete[] Slots;
        delete[] Workers;
        delete[] Traces;
        delete[] Paths;
        if (GateReady)
        {
            Gate.Destroy();
        }
    }
    Slot* Find(RequestHandle handle) noexcept
    {
        if (handle.Scheduler != Identity || handle.Sequence == 0)
        {
            return nullptr;
        }
        for (uint32 index = 0; index < Config.MaxRequests; ++index)
        {
            auto& slot = Slots[index];
            if (slot.Occupied && slot.Completion.Handle.Sequence == handle.Sequence)
            {
                return &slot;
            }
        }
        return nullptr;
    }
    Slot* Select(uint64 now) noexcept
    {
        Slot* best = nullptr;
        Slot* oldest = nullptr;
        for (uint32 index = 0; index < Config.MaxRequests; ++index)
        {
            auto& slot = Slots[index];
            if (slot.Occupied == false || slot.State != RequestState::Queued)
            {
                continue;
            }
            if (oldest == nullptr || slot.AcceptedAt < oldest->AcceptedAt ||
                (slot.AcceptedAt == oldest->AcceptedAt &&
                 slot.Completion.Handle.Sequence < oldest->Completion.Handle.Sequence))
            {
                oldest = &slot;
            }
            const uint64 deadline = slot.Deadline == 0 ? MAX_COUNTER : slot.Deadline;
            const uint64 bestDeadline = best == nullptr || best->Deadline == 0 ? MAX_COUNTER : best->Deadline;
            if (best == nullptr || deadline < bestDeadline ||
                (deadline == bestDeadline && slot.Priority > best->Priority) ||
                (deadline == bestDeadline && slot.Priority == best->Priority &&
                 slot.Completion.Handle.Sequence < best->Completion.Handle.Sequence))
            {
                best = &slot;
            }
        }
        return oldest != nullptr && now - oldest->AcceptedAt >= Config.StarvationNanoseconds ? oldest : best;
    }
    void Finish(Slot& slot, ReadDisposition disposition, ReadResult result, uint64 now) noexcept
    {
        slot.State = RequestState::Completing;
        slot.TerminalOrder = NextTerminal++;
        slot.Completion.Disposition = disposition;
        slot.Completion.Read = result;
        slot.Completion.ServiceNanoseconds = now - slot.StartedAt;
        ++Counters.Completing;
        switch (disposition)
        {
            case ReadDisposition::Read:
                Add(Counters.Reads);
                Add(Counters.BytesRead, result.BytesRead);
                break;
            case ReadDisposition::Cancelled:
                Add(Counters.Cancelled);
                break;
            case ReadDisposition::DeadlineExpired:
                Add(Counters.Expired);
                break;
        }
    }
    static void Run(void* context) noexcept
    {
        auto& state = *static_cast<Impl*>(context);
        gReadingScheduler = &state;
        for (;;)
        {
            state.Gate.Lock();
            Slot* slot = nullptr;
            while ((slot = state.Select(AsyncNow())) == nullptr && state.Stopping == false)
            {
                state.Gate.Wait();
            }
            if (slot == nullptr)
            {
                state.Gate.Unlock();
                return;
            }
            slot->StartedAt = AsyncNow();
            slot->Completion.QueueNanoseconds = slot->StartedAt - slot->AcceptedAt;
            --state.Counters.Queued;
            slot->State = RequestState::Submitted;
            if (slot->Cancellation || (slot->Deadline != 0 && slot->StartedAt >= slot->Deadline))
            {
                state.Finish(*slot,
                             slot->Cancellation ? ReadDisposition::Cancelled : ReadDisposition::DeadlineExpired,
                             {},
                             slot->StartedAt);
                state.Gate.Unlock();
                continue;
            }
            ++state.Counters.Submitted;
            state.Gate.Unlock();
            const ReadResult read = slot->File.ReadAt(slot->Offset, slot->Destination);
            state.Gate.Lock();
            --state.Counters.Submitted;
            state.Finish(*slot,
                         slot->Cancellation ? ReadDisposition::Cancelled : ReadDisposition::Read,
                         slot->Cancellation ? ReadResult{} : read,
                         AsyncNow());
            state.Gate.Unlock();
        }
    }
    void Stop() noexcept
    {
        {
            detail::IoLock lock(Gate);
            Stopping = true;
            for (uint32 index = 0; index < Config.MaxRequests; ++index)
            {
                auto& slot = Slots[index];
                if (slot.Occupied && slot.State == RequestState::Queued && slot.Cancellation == false)
                {
                    slot.Cancellation = true;
                    slot.CancelledAt = AsyncNow();
                }
            }
            Gate.WakeAll();
        }
        for (uint32 index = 0; index < StartedWorkers; ++index)
        {
            Workers[index].Join();
        }
        StartedWorkers = 0;
    }
};

AsyncReader::~AsyncReader() noexcept
{
    if (mImpl != nullptr)
    {
        mImpl->Stop();
        delete mImpl;
    }
}
AsyncStatus AsyncReader::Initialize(AsyncConfig config) noexcept
{
    if (mImpl != nullptr)
    {
        return AsyncStatus::AlreadyInitialized;
    }
    if (config.MaxRequests == 0 || config.MaxRequests > 4096 || config.WorkerCount == 0 || config.WorkerCount > 64 ||
        config.MaxBytes == 0 || config.StarvationNanoseconds == 0 || config.TraceCapacity > 4096)
    {
        return AsyncStatus::InvalidArgument;
    }
#if defined(LUDUS_PLATFORM_WEB)
    return AsyncStatus::Unsupported;
#else
    auto* state = LUDUS_ASYNC_FAIL(State) ? nullptr : new (std::nothrow) Impl;
    if (state == nullptr)
    {
        return AsyncStatus::OutOfMemory;
    }
    state->Config = config;
    state->Slots = LUDUS_ASYNC_FAIL(Slots) ? nullptr : new (std::nothrow) Impl::Slot[config.MaxRequests];
    state->Workers = LUDUS_ASYNC_FAIL(Workers) ? nullptr : new (std::nothrow) detail::IoWorker[config.WorkerCount];
    if (config.TraceCapacity != 0)
    {
        state->Traces = LUDUS_ASYNC_FAIL(Traces) ? nullptr : new (std::nothrow) ReadTrace[config.TraceCapacity];
        state->Paths = LUDUS_ASYNC_FAIL(Paths) ? nullptr : new (std::nothrow) VirtualPath[config.MaxRequests];
    }
    if (state->Slots == nullptr || state->Workers == nullptr ||
        (config.TraceCapacity != 0 && (state->Traces == nullptr || state->Paths == nullptr)))
    {
        delete state;
        return AsyncStatus::OutOfMemory;
    }
    uint64 identity = gNextScheduler.load(std::memory_order_relaxed);
    while (identity != MAX_COUNTER &&
           gNextScheduler.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed) == false)
    {
    }
    if (identity == MAX_COUNTER)
    {
        delete state;
        return AsyncStatus::IdentityExhausted;
    }
    state->Identity = identity;
    if (LUDUS_ASYNC_FAIL(Gate) || state->Gate.Initialize() == false)
    {
        delete state;
        return AsyncStatus::ThreadCreationFailed;
    }
    state->GateReady = true;
    for (uint32 index = 0; index < config.WorkerCount; ++index)
    {
        if (LUDUS_ASYNC_FAIL(Start) || state->Workers[index].Start(Impl::Run, state) == false)
        {
            state->Stop();
            delete state;
            return AsyncStatus::ThreadCreationFailed;
        }
        ++state->StartedWorkers;
    }
    mImpl = state;
    return AsyncStatus::Ok;
#endif
}
AsyncStatus AsyncReader::Submit(const VirtualFile& file,
                                uint64 offset,
                                std::span<uint8> destination,
                                RequestHandle& handle,
                                ReadOptions options) noexcept
{
    if (mImpl == nullptr)
    {
        return AsyncStatus::NotInitialized;
    }
    auto& state = *mImpl;
    detail::IoLock lock(state.Gate);
    const auto reject = [&state](AsyncStatus status) noexcept {
        Add(state.Counters.Rejected);
        return status;
    };
    if (state.Stopping)
    {
        return reject(AsyncStatus::Stopped);
    }
    if (file.IsOpen() == false || offset > file.Size() ||
        (options.TracePath != nullptr && options.TracePath->IsValid() == false))
    {
        return reject(AsyncStatus::InvalidArgument);
    }
    if (state.NextSequence == MAX_COUNTER)
    {
        return reject(AsyncStatus::IdentityExhausted);
    }
    if (state.Counters.InFlight == state.Config.MaxRequests)
    {
        return reject(AsyncStatus::CapacityExceeded);
    }
    if (destination.size() > state.Config.MaxBytes - state.Counters.BytesInFlight)
    {
        return reject(AsyncStatus::ByteBudgetExceeded);
    }
    for (uint32 index = 0; index < state.Config.MaxRequests; ++index)
    {
        auto& slot = state.Slots[index];
        if (slot.Occupied)
        {
            continue;
        }
        // Clone is a retain, not a descriptor duplication or allocation.
        if (file.Clone(slot.File).Succeeded() == false)
        {
            return reject(AsyncStatus::InvalidArgument);
        }
        slot.Destination = destination;
        slot.Offset = offset;
        slot.Deadline = options.DeadlineNanoseconds;
        slot.Priority = options.Priority;
        slot.AcceptedAt = AsyncNow();
        slot.CancelledAt = 0;
        slot.Cancellation = false;
        slot.Completion = {};
        slot.Completion.Handle = {state.Identity, state.NextSequence++};
        slot.Completion.Tag = options.Tag;
        slot.Completion.MountId = file.MountId();
        slot.Completion.Generation = file.Generation();
        slot.Occupied = true;
        slot.State = RequestState::Queued;
        if (state.Paths != nullptr)
        {
            state.Paths[index] = options.TracePath == nullptr ? VirtualPath{} : *options.TracePath;
        }
        Add(state.Counters.Accepted);
        ++state.Counters.InFlight;
        ++state.Counters.Queued;
        state.Counters.BytesInFlight += destination.size();
        if (state.Counters.InFlight > state.Counters.PeakRequests)
        {
            state.Counters.PeakRequests = state.Counters.InFlight;
        }
        if (state.Counters.BytesInFlight > state.Counters.PeakBytes)
        {
            state.Counters.PeakBytes = state.Counters.BytesInFlight;
        }
        handle = slot.Completion.Handle;
        state.Gate.WakeAll();
        return AsyncStatus::Ok;
    }
    return reject(AsyncStatus::CapacityExceeded);
}
AsyncStatus AsyncReader::Cancel(RequestHandle handle) noexcept
{
    if (mImpl == nullptr)
    {
        return AsyncStatus::NotInitialized;
    }
    detail::IoLock lock(mImpl->Gate);
    auto* slot = mImpl->Find(handle);
    if (slot == nullptr)
    {
        return AsyncStatus::InvalidHandle;
    }
    if (slot->State != RequestState::Completing && slot->Cancellation == false)
    {
        slot->Cancellation = true;
        slot->CancelledAt = AsyncNow();
        mImpl->Gate.WakeAll();
    }
    return AsyncStatus::Ok;
}
AsyncStatus AsyncReader::GetState(RequestHandle handle, RequestState& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return AsyncStatus::NotInitialized;
    }
    detail::IoLock lock(mImpl->Gate);
    auto* slot = mImpl->Find(handle);
    if (slot == nullptr)
    {
        return AsyncStatus::InvalidHandle;
    }
    output = slot->State;
    return AsyncStatus::Ok;
}
bool AsyncReader::Poll(ReadCompletion& output) noexcept
{
    if (mImpl == nullptr)
    {
        return false;
    }
    auto& state = *mImpl;
    VirtualFile release;
    {
        detail::IoLock lock(state.Gate);
        Impl::Slot* best = nullptr;
        uint32 bestIndex = 0;
        for (uint32 index = 0; index < state.Config.MaxRequests; ++index)
        {
            auto& slot = state.Slots[index];
            if (slot.Occupied && slot.State == RequestState::Completing &&
                (best == nullptr || slot.TerminalOrder < best->TerminalOrder))
            {
                best = &slot;
                bestIndex = index;
            }
        }
        if (best == nullptr)
        {
            return false;
        }
        const uint64 now = AsyncNow();
        best->State = RequestState::Completed;
        best->Completion.CompletionNanoseconds = now - best->AcceptedAt;
        best->Completion.CancellationNanoseconds = best->CancelledAt == 0 ? 0 : now - best->CancelledAt;
        output = best->Completion;
        if (state.Traces != nullptr)
        {
            if (state.TraceCount == state.Config.TraceCapacity)
            {
                state.TraceHead = (state.TraceHead + 1) % state.Config.TraceCapacity;
                --state.TraceCount;
                Add(state.Counters.DroppedTraces);
            }
            auto& trace = state.Traces[(state.TraceHead + state.TraceCount++) % state.Config.TraceCapacity];
            trace.Completion = output;
            trace.Path = state.Paths[bestIndex];
            trace.Offset = best->Offset;
            trace.RequestedBytes = best->Destination.size();
        }
        --state.Counters.InFlight;
        --state.Counters.Completing;
        state.Counters.BytesInFlight -= best->Destination.size();
        release = Move(best->File);
        best->Destination = {};
        best->Occupied = false;
    }
    // Provider destruction is outside the scheduler gate; a provider may be reentrant.
    return true;
}
bool AsyncReader::PollTrace(ReadTrace& output) noexcept
{
    if (mImpl == nullptr)
    {
        return false;
    }
    detail::IoLock lock(mImpl->Gate);
    if (mImpl->TraceCount == 0)
    {
        return false;
    }
    output = mImpl->Traces[mImpl->TraceHead];
    mImpl->TraceHead = (mImpl->TraceHead + 1) % mImpl->Config.TraceCapacity;
    --mImpl->TraceCount;
    return true;
}
AsyncStatus AsyncReader::Metrics(AsyncMetrics& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return AsyncStatus::NotInitialized;
    }
    detail::IoLock lock(mImpl->Gate);
    output = mImpl->Counters;
    const uint64 now = AsyncNow();
    for (uint32 index = 0; index < mImpl->Config.MaxRequests; ++index)
    {
        const auto& slot = mImpl->Slots[index];
        if (slot.Occupied && slot.State == RequestState::Queued &&
            now - slot.AcceptedAt > output.OldestQueueNanoseconds)
        {
            output.OldestQueueNanoseconds = now - slot.AcceptedAt;
        }
    }
    return AsyncStatus::Ok;
}
AsyncStatus AsyncReader::Shutdown() noexcept
{
    if (mImpl == nullptr)
    {
        return AsyncStatus::NotInitialized;
    }
    if (gReadingScheduler == mImpl)
    {
        return AsyncStatus::InsideRead;
    }
    mImpl->Stop();
    return AsyncStatus::Ok;
}
} // namespace ludus::foundation::filesystem
