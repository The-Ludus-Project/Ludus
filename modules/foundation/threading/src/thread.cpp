#include <ludus/foundation/threading/thread.hpp>

#include "internal/native.hpp"

#include <new>
#include <type_traits>

namespace ludus::foundation::threading
{
namespace
{
static_assert(sizeof(detail::NativeThread) <= Thread::NATIVE_STORAGE_BYTES,
              "Thread::NATIVE_STORAGE_BYTES must hold the native thread record");
static_assert(alignof(detail::NativeThread) <= alignof(void*), "the native thread record is pointer aligned");
static_assert(std::is_trivially_destructible_v<detail::NativeThread>,
              "Thread releases its native record without a destructor call");

detail::NativeThread& GetNativeThread(uint8* storage) noexcept
{
    return *std::launder(reinterpret_cast<detail::NativeThread*>(storage));
}
} // namespace

Thread::~Thread() noexcept
{
    LUDUS_ASSERT(mIsRunning == false);
}

ThreadStatus Thread::Start(ThreadEntry entry, void* contextOrNull) noexcept
{
    if (entry == nullptr)
    {
        return ThreadStatus::InvalidArgument;
    }
    if (mIsRunning == true)
    {
        return ThreadStatus::InvalidState;
    }
    detail::NativeThread* native = ::new (static_cast<void*>(mNativeStorage)) detail::NativeThread{};
    if (native->Start(entry, contextOrNull) == false)
    {
        return ThreadStatus::Unavailable;
    }
    mIsRunning = true;
    return ThreadStatus::Ok;
}

ThreadStatus Thread::Join() noexcept
{
    if (mIsRunning == false)
    {
        return ThreadStatus::InvalidState;
    }
    GetNativeThread(mNativeStorage).Join();
    mIsRunning = false;
    releaseCallable();
    return ThreadStatus::Ok;
}

void SleepFor(time::Duration interval) noexcept
{
    detail::NativeSleep(interval.Nanoseconds);
}

void YieldThread() noexcept
{
    detail::NativeYield();
}

ThreadId GetCallingThreadId() noexcept
{
    return {detail::NativeCallingThreadId()};
}
} // namespace ludus::foundation::threading
