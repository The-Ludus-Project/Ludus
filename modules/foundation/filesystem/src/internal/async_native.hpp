#pragma once

#include <ludus/foundation/base/core.h>

#if defined(LUDUS_PLATFORM_WINDOWS)
#    include <windows.h>
#elif !defined(LUDUS_PLATFORM_WEB)
#    include <pthread.h>
#endif

namespace ludus::foundation::filesystem::detail
{
// Private fallible OS boundary: std::thread's throwing constructor is unsuitable.
struct IoGate
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    CRITICAL_SECTION Mutex{};
    CONDITION_VARIABLE Condition{};
#elif !defined(LUDUS_PLATFORM_WEB)
    pthread_mutex_t Mutex{};
    pthread_cond_t Condition{};
#endif
    [[nodiscard]] bool Initialize() noexcept;
    void Destroy() noexcept;
    void Lock() noexcept;
    void Unlock() noexcept;
    void Wait() noexcept;
    void WakeAll() noexcept;
};
struct IoWorker
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    HANDLE HandleOrNull = nullptr;
#elif !defined(LUDUS_PLATFORM_WEB)
    pthread_t Handle{};
#endif
    void (*Function)(void*) noexcept = nullptr;
    void* Context = nullptr;
    [[nodiscard]] bool Start(void (*function)(void*) noexcept, void* context) noexcept;
    void Join() noexcept;
};
class IoLock final
{
public:
    explicit IoLock(IoGate& gate) noexcept : mGate(gate)
    {
        mGate.Lock();
    }
    ~IoLock() noexcept
    {
        mGate.Unlock();
    }
    IoLock(const IoLock&) = delete;
    IoLock& operator=(const IoLock&) = delete;

private:
    IoGate& mGate;
};
} // namespace ludus::foundation::filesystem::detail
