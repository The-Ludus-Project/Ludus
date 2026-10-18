#include <ludus/foundation/base/types.h>

#if defined(LUDUS_CONTROL_TSAN)
#    include <atomic>
#    include <pthread.h>

namespace
{
// Keep the control slot independent of thread-wrapper allocation/lifetime state.
alignas(64) volatile ludus::foundation::int32 gRaceValue = 0;
alignas(64) std::atomic<bool> gParentWritten{false};
// Bound the repeated conflicting accesses independently of scheduler timing.
constexpr ludus::foundation::usize CONTROL_WRITES = 4096;

void* WriteRaceValue(void*) noexcept
{
    // Relaxed polling orders wall-clock execution without synchronizing the slot.
    // Thanks to LLVM's compiler-rt, tsan_interface_atomic.cpp (Clang 18.1.8),
    // AtomicLoad/AtomicStore relaxed fast paths, for confirming no clock acquire
    // or release is added for this test-only rendezvous.
    // https://github.com/llvm/llvm-project/blob/llvmorg-18.1.8/compiler-rt/lib/tsan/rtl/tsan_interface_atomic.cpp
    while (gParentWritten.load(std::memory_order_relaxed) == false)
    {
    }
    for (ludus::foundation::usize i = 0; i < CONTROL_WRITES; ++i)
    {
        gRaceValue = 1;
    }
    return nullptr;
}
} // namespace
#endif

// Deliberate test-only defects. Run through sanitizer_control.py, which checks
// both a clean startup and the specific fatal diagnostic in separate processes.
// Thanks to LLVM, "UndefinedBehaviorSanitizer", Clang 18.1.8, Usage, for the
// signed-overflow control; adapted to Ludus types and volatile input/output.
// https://releases.llvm.org/18.1.8/tools/clang/docs/UndefinedBehaviorSanitizer.html
int main(int argumentCount, char**)
{
    using ludus::foundation::int32;
    if (argumentCount == 1)
    {
        return 0;
    }

#if defined(LUDUS_CONTROL_ASAN)
    volatile int32 index = argumentCount - 1;
    volatile int32* values = new int32[1];
    values[index] = 7;
    volatile int32 observed = values[index];
    delete[] values;
    return observed == 7 ? 0 : 1;
#elif defined(LUDUS_CONTROL_UBSAN)
    volatile int32 value = 0x7fffffff;
    value = value + argumentCount;
    return value == 0 ? 1 : 0;
#elif defined(LUDUS_CONTROL_TSAN)
    // Thanks to LLVM, "ThreadSanitizer", Clang 18.1.8, Usage (tiny_race.c),
    // for conflicting parent/worker writes before join. Keep its direct pthread
    // creation/join topology, adapted to bounded Ludus volatile writes and explicit errors.
    // https://releases.llvm.org/18.1.8/tools/clang/docs/ThreadSanitizer.html
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, WriteRaceValue, nullptr) != 0)
    {
        return 1;
    }
    // Force the worker's conflicting access to observe the live parent's write.
    // A release/acquire gate here would order the writes and remove the defect.
    for (ludus::foundation::usize i = 0; i < CONTROL_WRITES; ++i)
    {
        gRaceValue = 2;
    }
    gParentWritten.store(true, std::memory_order_relaxed);
    if (pthread_join(worker, nullptr) != 0)
    {
        return 1;
    }
    return gRaceValue == 0 ? 1 : 0;
#else
#    error A sanitizer control kind is required
#endif
}
