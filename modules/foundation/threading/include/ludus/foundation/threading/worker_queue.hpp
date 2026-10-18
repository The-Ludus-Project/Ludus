#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>

namespace ludus::foundation::threading
{
/// Maximum accepted tasks, including running and uncollected completions.
inline constexpr usize MAX_WORKER_QUEUE_TASKS = 64;
/// Admission/lifecycle result for a dedicated worker queue.
enum class WorkerQueueStatus : uint8
{
    Ready,            ///< Operation succeeded.
    InvalidArgument,  ///< A required callback is null.
    InvalidState,     ///< Not initialized, already initialized, or called from a callback.
    CapacityExceeded, ///< All task slots await execution or completion collection.
    OutOfMemory,      ///< Queue storage could not be allocated.
    Unavailable,      ///< Native synchronization/thread startup failed; browser is unsupported.
};
/// A nonthrowing task or completion function; context is borrowed until completion returns.
using WorkerQueueFunction = void (*)(void* contextOrNull) noexcept;
/// One dedicated native execution lane with owner-thread completion delivery.
/// Unlike JobSystem, it never helps execute work on the calling thread. Suitable
/// for blocking driver/I/O preparation isolated from the general CPU job pool.
/// All public calls and destruction use one owner thread. Execute callbacks run
/// on the worker; completion callbacks run only in Poll/Shutdown on the owner.
/// Callbacks must not reenter this queue. Initialization may allocate/start a thread;
/// Submit/Poll allocate nothing and never wait for task execution. Browser has no fallback.
class WorkerQueue final
{
public:
    WorkerQueue() noexcept;                   ///< Construct without allocation or native threads.
    ~WorkerQueue() noexcept;                  ///< Drain accepted tasks and deliver every completion on the owner.
    WorkerQueue(const WorkerQueue&) = delete; ///< Queue ownership is unique.
    WorkerQueue& operator=(const WorkerQueue&) = delete; ///< Queue ownership is unique.
    /// Allocate fixed storage/start one worker; failure leaves the queue uninitialized.
    [[nodiscard]] WorkerQueueStatus Initialize() noexcept;
    /// Copy callbacks/context into a bounded FIFO. Ready admits ownership of the
    /// borrowed context lifetime through completion. Other results call neither callback.
    [[nodiscard]] WorkerQueueStatus
    Submit(WorkerQueueFunction execute, WorkerQueueFunction complete, void* contextOrNull) noexcept;
    /// Deliver at most one finished task. False leaves queued/running tasks alone.
    /// Completion sees every write made by its execute callback.
    [[nodiscard]] bool Poll() noexcept;
    /// Shutdown-only blocking fence: finish accepted work, join the worker, deliver
    /// all completions and release storage. Idempotent; InvalidState from a callback.
    [[nodiscard]] WorkerQueueStatus Shutdown() noexcept;

private:
    struct State;
    core::UniquePtr<State> mState;
};
} // namespace ludus::foundation::threading
