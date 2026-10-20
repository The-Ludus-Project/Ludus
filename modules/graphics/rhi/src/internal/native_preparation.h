#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/worker_queue.hpp>

#if defined(LUDUS_RHI_TEST_READBACK)
#    include <atomic>
#endif

// Private native backend boundary. Worker tasks touch only captured inputs and
// their own result. Resource tables, status, budgets and retirement stay on the
// owner; shutdown drains before releasing any device or captured dependency.
namespace ludus::graphics::rhi::backend
{
namespace
{
using foundation::core::UniquePtr;
namespace threading = foundation::threading;
struct NativePreparationTask
{
    foundation::usize AllocationBytes = 0;
    foundation::usize QueueSlot = 0;
    foundation::usize ResourceSlot = 0;
    foundation::uint32 Request = 0;
    bool IsPipelinePreparation = false;
    internal::RasterKind Kind = internal::RasterKind::Shader;
    virtual ~NativePreparationTask() noexcept
    {
        internal::ReleaseHostStorage(AllocationBytes);
    }
    virtual void Execute() noexcept = 0;
    virtual RasterStatus Complete(bool isDraining) noexcept = 0;
};
threading::WorkerQueue gPreparationQueue;
foundation::StaticArray<UniquePtr<NativePreparationTask>, threading::MAX_WORKER_QUEUE_TASKS> gPreparationTasks{};
bool isPreparationInitialized = false;
bool isPreparationDraining = false;
#if defined(LUDUS_RHI_TEST_READBACK)
std::atomic<bool> isPreparationPaused{false};
std::atomic<foundation::uint32> gPreparationStarted{0};
#endif
void ExecuteNativePreparation(void* contextOrNull) noexcept
{
    LUDUS_REQUIRE(contextOrNull != nullptr);
#if defined(LUDUS_RHI_TEST_READBACK)
    if (static_cast<NativePreparationTask*>(contextOrNull)->IsPipelinePreparation == true)
    {
        gPreparationStarted.fetch_add(1, std::memory_order_release);
        while (isPreparationPaused.load(std::memory_order_acquire) == true)
        {
            isPreparationPaused.wait(true, std::memory_order_acquire);
        }
    }
#endif
    static_cast<NativePreparationTask*>(contextOrNull)->Execute();
}
void CompleteNativePreparation(void* contextOrNull) noexcept
{
    LUDUS_REQUIRE(contextOrNull != nullptr);
    NativePreparationTask& task = *static_cast<NativePreparationTask*>(contextOrNull);
    if (internal::IsRasterRequestCurrent(
            { .Kind = task.Kind, .ResourceSlot = task.ResourceSlot, .Request = task.Request }) == true)
    {
        static_cast<void>(task.Complete(isPreparationDraining));
    }
    gPreparationTasks[task.QueueSlot].Reset();
}
template <typename T>
[[nodiscard]] UniquePtr<T> MakePreparationTask() noexcept
{
    if (internal::ReserveHostStorage(sizeof(T)) == false)
    {
        return {};
    }
    T* allocationOrNull = new (std::nothrow) T;
    UniquePtr<T> task{allocationOrNull};
    if (task.Get() == nullptr)
    {
        internal::ReleaseHostStorage(sizeof(T));
        internal::StorageAllocationFailed();
    }
    else
    {
        task->AllocationBytes = sizeof(T);
    }
    return task;
}
RasterStatus SubmitNativePreparation(UniquePtr<NativePreparationTask> task, bool shouldPrepareAsynchronously) noexcept
{
    if (shouldPrepareAsynchronously == false)
    {
        internal::RasterExpect(task->Request, internal::RasterCallbacks::One);
        task->Execute();
        return task->Complete(false);
    }
    if (isPreparationInitialized == false)
    {
        const threading::WorkerQueueStatus started = gPreparationQueue.Initialize();
        if (started != threading::WorkerQueueStatus::Ready)
        {
            return started == threading::WorkerQueueStatus::OutOfMemory ? RasterStatus::OutOfMemory
                                                                        : RasterStatus::Failed;
        }
        isPreparationInitialized = true;
    }
    for (foundation::usize i = 0; i < gPreparationTasks.GetSize(); ++i)
    {
        if (gPreparationTasks[i].Get() != nullptr)
        {
            continue;
        }
        task->QueueSlot = i;
        const threading::WorkerQueueStatus admitted =
            gPreparationQueue.Submit(ExecuteNativePreparation, CompleteNativePreparation, task.Get());
        if (admitted != threading::WorkerQueueStatus::Ready)
        {
            return RasterStatus::CapacityExceeded;
        }
        internal::RasterExpect(task->Request, internal::RasterCallbacks::One);
        gPreparationTasks[i] = foundation::Move(task);
        return RasterStatus::Pending;
    }
    internal::StorageCapacityFailed();
    return RasterStatus::CapacityExceeded;
}
void PollNativePreparation() noexcept
{
    for (foundation::usize i = 0; i < threading::MAX_WORKER_QUEUE_TASKS; ++i)
    {
        if (gPreparationQueue.Poll() == false)
        {
            break;
        }
    }
}
void DrainNativePreparation() noexcept
{
#if defined(LUDUS_RHI_TEST_READBACK)
    isPreparationPaused.store(false, std::memory_order_release);
    isPreparationPaused.notify_all();
#endif
    isPreparationDraining = true;
    LUDUS_REQUIRE(gPreparationQueue.Shutdown() == threading::WorkerQueueStatus::Ready);
    isPreparationInitialized = false;
    isPreparationDraining = false;
}
} // namespace
#if defined(LUDUS_RHI_TEST_READBACK)
void SetNativePipelinePreparationPaused(bool isPaused) noexcept
{
    isPreparationPaused.store(isPaused, std::memory_order_release);
    isPreparationPaused.notify_all();
}
foundation::uint32 GetNativePreparationStarted() noexcept
{
    return gPreparationStarted.load(std::memory_order_acquire);
}
#endif
} // namespace ludus::graphics::rhi::backend
