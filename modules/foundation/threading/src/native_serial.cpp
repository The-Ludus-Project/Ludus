#include "internal/native.hpp"

namespace ludus::foundation::threading::detail
{
namespace
{
/// Identity reported for the single thread of the serial backend.
constexpr uint64 SERIAL_THREAD_ID = 1;
} // namespace

bool NativeGate::Initialize() noexcept
{
    return true;
}
void NativeGate::Destroy() noexcept {}
void NativeGate::Lock() noexcept {}
void NativeGate::Unlock() noexcept {}
void NativeGate::Wait() noexcept
{
    LUDUS_FATAL("Serial executor cannot sleep waiting for work");
}
void NativeGate::WakeAll() noexcept {}
bool NativeThread::Start(ThreadFunction, void*) noexcept
{
    return false;
}
void NativeThread::Join() noexcept {}
void NativeSleep(uint64) noexcept {}
void NativeYield() noexcept {}
uint64 NativeCallingThreadId() noexcept
{
    // The serial backend runs everything on the one browser thread.
    return SERIAL_THREAD_ID;
}
} // namespace ludus::foundation::threading::detail
