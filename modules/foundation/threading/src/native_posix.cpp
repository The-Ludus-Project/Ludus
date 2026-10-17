#include "internal/native.hpp"

#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
#    include "internal/native_test_hooks.hpp"
#endif

namespace ludus::foundation::threading::detail
{
bool NativeGate::Initialize() noexcept
{
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    if (test::ShouldFailMutexInitialization() == true)
    {
        return false;
    }
#endif
    if (pthread_mutex_init(&Mutex, nullptr) != 0)
    {
        return false;
    }
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    if (test::ShouldFailConditionInitialization() == true)
    {
        (void)pthread_mutex_destroy(&Mutex);
        return false;
    }
#endif
    if (pthread_cond_init(&Condition, nullptr) != 0)
    {
        (void)pthread_mutex_destroy(&Mutex);
        return false;
    }
    return true;
}
void NativeGate::Destroy() noexcept
{
    (void)pthread_cond_destroy(&Condition);
    (void)pthread_mutex_destroy(&Mutex);
}
void NativeGate::Lock() noexcept
{
    LUDUS_REQUIRE(pthread_mutex_lock(&Mutex) == 0);
}
void NativeGate::Unlock() noexcept
{
    LUDUS_REQUIRE(pthread_mutex_unlock(&Mutex) == 0);
}
void NativeGate::Wait() noexcept
{
    LUDUS_REQUIRE(pthread_cond_wait(&Condition, &Mutex) == 0);
}
void NativeGate::WakeAll() noexcept
{
    LUDUS_REQUIRE(pthread_cond_broadcast(&Condition) == 0);
}

namespace
{
void* ThreadEntry(void* context) noexcept
{
    NativeThread& thread = *static_cast<NativeThread*>(context);
    thread.Function(thread.ContextOrNull);
    return nullptr;
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
    return pthread_create(&Handle, nullptr, ThreadEntry, this) == 0;
}
void NativeThread::Join() noexcept
{
#if defined(LUDUS_THREADING_NATIVE_FAULT_TESTING)
    test::RecordThreadJoin();
#endif
    LUDUS_REQUIRE(pthread_join(Handle, nullptr) == 0);
}
} // namespace ludus::foundation::threading::detail
