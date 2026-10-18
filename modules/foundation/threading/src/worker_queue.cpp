#include <ludus/foundation/threading/worker_queue.hpp>

#include "internal/native.hpp"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <new>

namespace ludus::foundation::threading
{
namespace
{
thread_local const void* gExecutingQueueOrNull = nullptr;
struct Task final
{
    WorkerQueueFunction ExecuteOrNull = nullptr;
    WorkerQueueFunction CompleteOrNull = nullptr;
    void* ContextOrNull = nullptr;
    bool IsOccupied = false;
};
} // namespace
struct WorkerQueue::State final
{
    detail::NativeGate Gate;
    detail::NativeThread Thread;
    StaticArray<Task, MAX_WORKER_QUEUE_TASKS> Tasks{};
    StaticArray<usize, MAX_WORKER_QUEUE_TASKS> Queued{};
    StaticArray<usize, MAX_WORKER_QUEUE_TASKS> Completed{};
    usize QueueHead = 0;
    usize QueueCount = 0;
    usize CompleteHead = 0;
    usize CompleteCount = 0;
    bool ShouldStop = false;
    bool IsDelivering = false;
    bool IsJoined = false;

    void Run() noexcept
    {
        gExecutingQueueOrNull = this;
        Gate.Lock();
        for (;;)
        {
            while (QueueCount == 0 && ShouldStop == false)
            {
                Gate.Wait();
            }
            if (QueueCount == 0 && ShouldStop == true)
            {
                break;
            }
            const usize slot = Queued[QueueHead];
            QueueHead = (QueueHead + 1) % MAX_WORKER_QUEUE_TASKS;
            --QueueCount;
            const Task task = Tasks[slot];
            Gate.Unlock();
            task.ExecuteOrNull(task.ContextOrNull);
            Gate.Lock();
            Completed[(CompleteHead + CompleteCount) % MAX_WORKER_QUEUE_TASKS] = slot;
            ++CompleteCount;
        }
        Gate.Unlock();
        gExecutingQueueOrNull = nullptr;
    }
};
WorkerQueue::WorkerQueue() noexcept = default;
WorkerQueue::~WorkerQueue() noexcept
{
    LUDUS_REQUIRE(Shutdown() == WorkerQueueStatus::Ready);
}
WorkerQueueStatus WorkerQueue::Initialize() noexcept
{
    if (mState.Get() != nullptr)
    {
        return WorkerQueueStatus::InvalidState;
    }
#if defined(LUDUS_PLATFORM_WEB)
    return WorkerQueueStatus::Unavailable;
#else
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> state{allocationOrNull};
    if (state.Get() == nullptr)
    {
        return WorkerQueueStatus::OutOfMemory;
    }
    if (state->Gate.Initialize() == false)
    {
        return WorkerQueueStatus::Unavailable;
    }
    if (state->Thread.Start(
            [](void* contextOrNull) noexcept {
                LUDUS_REQUIRE(contextOrNull != nullptr);
                static_cast<State*>(contextOrNull)->Run();
            },
            state.Get()) == false)
    {
        state->Gate.Destroy();
        return WorkerQueueStatus::Unavailable;
    }
    mState = Move(state);
    return WorkerQueueStatus::Ready;
#endif
}
WorkerQueueStatus
WorkerQueue::Submit(WorkerQueueFunction execute, WorkerQueueFunction complete, void* contextOrNull) noexcept
{
    if (mState.Get() == nullptr || gExecutingQueueOrNull == mState.Get() || mState->IsDelivering == true ||
        mState->IsJoined == true)
    {
        return WorkerQueueStatus::InvalidState;
    }
    if (execute == nullptr || complete == nullptr)
    {
        return WorkerQueueStatus::InvalidArgument;
    }
    State& state = *mState;
    detail::GateLock lock{state.Gate};
    for (usize i = 0; i < MAX_WORKER_QUEUE_TASKS; ++i)
    {
        if (state.Tasks[i].IsOccupied == false)
        {
            state.Tasks[i] = {execute, complete, contextOrNull, true};
            state.Queued[(state.QueueHead + state.QueueCount) % MAX_WORKER_QUEUE_TASKS] = i;
            ++state.QueueCount;
            state.Gate.WakeAll();
            return WorkerQueueStatus::Ready;
        }
    }
    return WorkerQueueStatus::CapacityExceeded;
}
bool WorkerQueue::Poll() noexcept
{
    if (mState.Get() == nullptr || gExecutingQueueOrNull == mState.Get() || mState->IsDelivering == true)
    {
        return false;
    }
    State& state = *mState;
    state.Gate.Lock();
    if (state.CompleteCount == 0)
    {
        state.Gate.Unlock();
        return false;
    }
    const usize slot = state.Completed[state.CompleteHead];
    state.CompleteHead = (state.CompleteHead + 1) % MAX_WORKER_QUEUE_TASKS;
    --state.CompleteCount;
    const Task task = state.Tasks[slot];
    state.Tasks[slot] = {};
    state.Gate.Unlock();
    state.IsDelivering = true;
    task.CompleteOrNull(task.ContextOrNull);
    state.IsDelivering = false;
    return true;
}
WorkerQueueStatus WorkerQueue::Shutdown() noexcept
{
    if (mState.Get() == nullptr)
    {
        return WorkerQueueStatus::Ready;
    }
    if (gExecutingQueueOrNull == mState.Get() || mState->IsDelivering == true)
    {
        return WorkerQueueStatus::InvalidState;
    }
    State& state = *mState;
    state.Gate.Lock();
    state.ShouldStop = true;
    state.Gate.WakeAll();
    state.Gate.Unlock();
    state.Thread.Join();
    state.IsJoined = true;
    while (Poll() == true)
    {
    }
    state.Gate.Destroy();
    mState.Reset();
    return WorkerQueueStatus::Ready;
}
} // namespace ludus::foundation::threading
