#include <ludus/foundation/base/config.h>
#include <ludus/foundation/threading/worker_queue.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace ludus::foundation;
namespace threading = ludus::foundation::threading;
namespace
{
struct Context final
{
    std::atomic<bool> CanExecute{false};
    std::atomic<bool> IsWorkerDistinct{false};
    std::atomic<uint32> Executed{0};
    uint32 Completed = 0;
    bool IsCompletionOnOwner = true;
    std::thread::id Owner = std::this_thread::get_id();
};
struct ReleaseGuard final
{
    Context& Value;
    ~ReleaseGuard()
    {
        Value.CanExecute.store(true);
        Value.CanExecute.notify_all();
    }
};
void Execute(void* contextOrNull) noexcept
{
    Context& context = *static_cast<Context*>(contextOrNull);
    context.IsWorkerDistinct.store(std::this_thread::get_id() != context.Owner);
    while (context.CanExecute.load() == false)
    {
        context.CanExecute.wait(false);
    }
    context.Executed.fetch_add(1);
}
void Complete(void* contextOrNull) noexcept
{
    Context& context = *static_cast<Context*>(contextOrNull);
    context.IsCompletionOnOwner = context.IsCompletionOnOwner == true && std::this_thread::get_id() == context.Owner;
    ++context.Completed;
}
} // namespace
TEST_CASE("Dedicated queue never executes on the caller and bounds uncollected tasks", "[threading][worker]")
{
    threading::WorkerQueue queue;
#if defined(LUDUS_PLATFORM_WEB)
    CHECK(queue.Initialize() == threading::WorkerQueueStatus::Unavailable);
#else
    Context context;
    REQUIRE(queue.Initialize() == threading::WorkerQueueStatus::Ready);
    ReleaseGuard release{context};
    for (usize i = 0; i < threading::MAX_WORKER_QUEUE_TASKS; ++i)
    {
        REQUIRE(queue.Submit(Execute, Complete, &context) == threading::WorkerQueueStatus::Ready);
    }
    CHECK(queue.Submit(Execute, Complete, &context) == threading::WorkerQueueStatus::CapacityExceeded);
    CHECK(queue.Poll() == false);
    CHECK(context.Executed.load() == 0);
    context.CanExecute.store(true);
    context.CanExecute.notify_all();
    REQUIRE(queue.Shutdown() == threading::WorkerQueueStatus::Ready);
    CHECK(context.Executed.load() == threading::MAX_WORKER_QUEUE_TASKS);
    CHECK(context.Completed == threading::MAX_WORKER_QUEUE_TASKS);
    CHECK(context.IsWorkerDistinct.load() == true);
    CHECK(context.IsCompletionOnOwner == true);
    REQUIRE(queue.Initialize() == threading::WorkerQueueStatus::Ready);
    CHECK(queue.Submit(nullptr, Complete, &context) == threading::WorkerQueueStatus::InvalidArgument);
    CHECK(queue.Shutdown() == threading::WorkerQueueStatus::Ready);
#endif
}
