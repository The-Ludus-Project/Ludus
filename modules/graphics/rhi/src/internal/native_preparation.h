#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/worker_queue.hpp>

#include "owner.h"

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
/// @brief Worker queue and queued tasks of one device owner (R14c).
///
/// Each owner polls only its own queue, so a completion always runs bound to the owner whose
/// tables it updates. Workers never read the owner binding.
struct NativePreparationState final
{
    threading::WorkerQueue Queue;
    foundation::StaticArray<UniquePtr<NativePreparationTask>, threading::MAX_WORKER_QUEUE_TASKS> Tasks{};
    bool IsInitialized = false;
    bool IsDraining = false;
};
foundation::StaticArray<NativePreparationState, MAX_DEVICE_OWNERS> gPreparationStates{};
/// The preparation queue of the bound owner slot.
NativePreparationState& GetPreparation() noexcept
{
    return gPreparationStates[internal::GetBoundOwnerSlot()];
}
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
        static_cast<void>(task.Complete(GetPreparation().IsDraining));
    }
    GetPreparation().Tasks[task.QueueSlot].Reset();
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
    if (GetPreparation().IsInitialized == false)
    {
        const threading::WorkerQueueStatus started = GetPreparation().Queue.Initialize();
        if (started != threading::WorkerQueueStatus::Ready)
        {
            return started == threading::WorkerQueueStatus::OutOfMemory ? RasterStatus::OutOfMemory
                                                                        : RasterStatus::Failed;
        }
        GetPreparation().IsInitialized = true;
    }
    for (foundation::usize i = 0; i < GetPreparation().Tasks.GetSize(); ++i)
    {
        if (GetPreparation().Tasks[i].Get() != nullptr)
        {
            continue;
        }
        task->QueueSlot = i;
        const threading::WorkerQueueStatus admitted =
            GetPreparation().Queue.Submit(ExecuteNativePreparation, CompleteNativePreparation, task.Get());
        if (admitted != threading::WorkerQueueStatus::Ready)
        {
            return RasterStatus::CapacityExceeded;
        }
        internal::RasterExpect(task->Request, internal::RasterCallbacks::One);
        GetPreparation().Tasks[i] = foundation::Move(task);
        return RasterStatus::Pending;
    }
    internal::StorageCapacityFailed();
    return RasterStatus::CapacityExceeded;
}
void PollNativePreparation() noexcept
{
    for (foundation::usize i = 0; i < threading::MAX_WORKER_QUEUE_TASKS; ++i)
    {
        if (GetPreparation().Queue.Poll() == false)
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
    GetPreparation().IsDraining = true;
    LUDUS_REQUIRE(GetPreparation().Queue.Shutdown() == threading::WorkerQueueStatus::Ready);
    GetPreparation().IsInitialized = false;
    GetPreparation().IsDraining = false;
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
