#include "internal/async_native.hpp"

#include <process.h>

namespace ludus::foundation::filesystem::detail
{
bool IoGate::Initialize() noexcept
{
    InitializeConditionVariable(&Condition);
    return InitializeCriticalSectionEx(&Mutex, 0, 0) != 0;
}
void IoGate::Destroy() noexcept
{
    DeleteCriticalSection(&Mutex);
}
void IoGate::Lock() noexcept
{
    EnterCriticalSection(&Mutex);
}
void IoGate::Unlock() noexcept
{
    LeaveCriticalSection(&Mutex);
}
void IoGate::Wait() noexcept
{
    LUDUS_REQUIRE(SleepConditionVariableCS(&Condition, &Mutex, INFINITE) != 0);
}
void IoGate::WakeAll() noexcept
{
    WakeAllConditionVariable(&Condition);
}

namespace
{
uint32 __stdcall ThreadEntry(void* context) noexcept
{
    IoWorker& thread = *static_cast<IoWorker*>(context);
    thread.Function(thread.Context);
    return 0;
}
} // namespace

bool IoWorker::Start(void (*function)(void*) noexcept, void* context) noexcept
{
    Function = function;
    Context = context;
    HandleOrNull = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ThreadEntry, this, 0, nullptr));
    return HandleOrNull != nullptr;
}
void IoWorker::Join() noexcept
{
    LUDUS_REQUIRE(WaitForSingleObject(HandleOrNull, INFINITE) == WAIT_OBJECT_0);
    (void)CloseHandle(HandleOrNull);
    HandleOrNull = nullptr;
}
} // namespace ludus::foundation::filesystem::detail
