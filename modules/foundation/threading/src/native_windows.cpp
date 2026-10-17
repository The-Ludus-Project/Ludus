#include "internal/native.hpp"

#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
#    include "internal/native_test_hooks.hpp"
#endif

#include <process.h>

namespace ludus::foundation::threading::detail
{
bool NativeGate::Initialize() noexcept
{
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    // Windows condition initialization cannot fail; the seam still exercises
    // the kernel's cleanup of a gate whose initialization reported failure.
    if (test::ShouldFailMutexInitialization() == true || test::ShouldFailConditionInitialization() == true)
    {
        return false;
    }
#endif
    InitializeConditionVariable(&Condition);
    return InitializeCriticalSectionEx(&Mutex, 0, 0) != 0;
}
void NativeGate::Destroy() noexcept
{
    DeleteCriticalSection(&Mutex);
}
void NativeGate::Lock() noexcept
{
    EnterCriticalSection(&Mutex);
}
void NativeGate::Unlock() noexcept
{
    LeaveCriticalSection(&Mutex);
}
void NativeGate::Wait() noexcept
{
    LUDUS_REQUIRE(SleepConditionVariableCS(&Condition, &Mutex, INFINITE) != 0);
}
void NativeGate::WakeAll() noexcept
{
    WakeAllConditionVariable(&Condition);
}

namespace
{
uint32 __stdcall ThreadEntry(void* context) noexcept
{
    NativeThread& thread = *static_cast<NativeThread*>(context);
    thread.Function(thread.ContextOrNull);
    return 0;
}
} // namespace

bool NativeThread::Start(ThreadFunction function, void* contextOrNull) noexcept
{
    Function = function;
    ContextOrNull = contextOrNull;
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    if (test::ShouldFailThreadStart() == true)
    {
        return false;
    }
#endif
    Handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ThreadEntry, this, 0, nullptr));
    return Handle != nullptr;
}
void NativeThread::Join() noexcept
{
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    test::RecordThreadJoin();
#endif
    LUDUS_REQUIRE(WaitForSingleObject(Handle, INFINITE) == WAIT_OBJECT_0);
    (void)CloseHandle(Handle);
    Handle = nullptr;
}
} // namespace ludus::foundation::threading::detail
