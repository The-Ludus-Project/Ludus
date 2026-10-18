#include <ludus/foundation/filesystem/async.hpp>

#include "internal/async_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#if defined(LUDUS_PLATFORM_LINUX)
#    include <cerrno>
#    include <pthread.h>
#endif

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
detail::AsyncFault gPoint = detail::AsyncFault::State;
uint32 gFailAt = 0;
uint32 gCalls = 0;
} // namespace
namespace ludus::foundation::filesystem::detail
{
bool FailAsync(AsyncFault point) noexcept
{
    return point == gPoint && ++gCalls == gFailAt;
}
} // namespace ludus::foundation::filesystem::detail
TEST_CASE("Every async allocation and partial worker startup failure cleans up and permits retry")
{
    for (const auto point : {detail::AsyncFault::State,
                             detail::AsyncFault::Slots,
                             detail::AsyncFault::Workers,
                             detail::AsyncFault::Traces,
                             detail::AsyncFault::Paths,
                             detail::AsyncFault::Gate,
                             detail::AsyncFault::Start})
    {
        for (const uint32 failAt : {1u, 2u, 4u})
        {
            if (point != detail::AsyncFault::Start && failAt != 1)
            {
                continue;
            }
            AsyncReader reader;
            gPoint = point;
            gCalls = 0;
            gFailAt = failAt;
            const auto status = reader.Initialize({4, 4, 16, 1, 2});
            gFailAt = 0;
            REQUIRE(status == (point == detail::AsyncFault::Gate || point == detail::AsyncFault::Start
                                   ? AsyncStatus::ThreadCreationFailed
                                   : AsyncStatus::OutOfMemory));
            AsyncMetrics metrics;
            metrics.Accepted = 999;
            REQUIRE(reader.Metrics(metrics) == AsyncStatus::NotInitialized);
            REQUIRE(metrics.Accepted == 999);
            REQUIRE(reader.Initialize({4, 2, 16, 1, 2}) == AsyncStatus::Ok);
            REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
        }
    }
}
TEST_CASE("Async validates configuration and preserves rejected outputs")
{
    AsyncReader reader;
    for (const AsyncConfig config : {AsyncConfig{0, 1, 1, 1, 0},
                                     AsyncConfig{4097, 1, 1, 1, 0},
                                     AsyncConfig{1, 0, 1, 1, 0},
                                     AsyncConfig{1, 65, 1, 1, 0},
                                     AsyncConfig{1, 1, 0, 1, 0},
                                     AsyncConfig{1, 1, 1, 0, 0},
                                     AsyncConfig{1, 1, 1, 1, 4097}})
    {
        REQUIRE(reader.Initialize(config) == AsyncStatus::InvalidArgument);
    }
    RequestHandle handle{7, 8};
    VirtualFile closed;
    REQUIRE(reader.Submit(closed, 0, {}, handle) == AsyncStatus::NotInitialized);
    REQUIRE(reader.Shutdown() == AsyncStatus::NotInitialized);
    REQUIRE(reader.Initialize({1, 1, 1, 1, 0}) == AsyncStatus::Ok);
    REQUIRE(reader.Initialize() == AsyncStatus::AlreadyInitialized);
    REQUIRE(reader.Submit(closed, 0, {}, handle) == AsyncStatus::InvalidArgument);
    REQUIRE(handle.Scheduler == 7);
    REQUIRE(handle.Sequence == 8);
}

#if defined(LUDUS_FILESYSTEM_ASYNC_ALLOCATION_TESTING)
namespace
{
std::atomic<uint32> gAllocations{0};
std::atomic<bool> gTracking{false};
void* Allocate(usize bytes) noexcept
{
    if (gTracking.load())
    {
        ++gAllocations;
    }
    return std::malloc(bytes == 0 ? 1 : bytes);
}
} // namespace
// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(usize bytes)
{
    if (auto* allocation = Allocate(bytes))
    {
        return allocation;
    }
    throw std::bad_alloc{};
}
void* operator new[](usize bytes)
{
    return ::operator new(bytes);
}
void* operator new(usize bytes, const std::nothrow_t&) noexcept
{
    return Allocate(bytes);
}
void* operator new[](usize bytes, const std::nothrow_t&) noexcept
{
    return Allocate(bytes);
}
void operator delete(void* allocation) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation) noexcept
{
    std::free(allocation);
}
void operator delete(void* allocation, usize) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation, usize) noexcept
{
    std::free(allocation);
}
void operator delete(void* allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

TEST_CASE("Warm async submission cancellation delivery tracing and metrics allocate nothing")
{
    const uint8 data[]{1, 2, 3};
    const MemoryEntry entry{"data", data};
    ProviderHandle provider;
    REQUIRE(CreateMemoryProvider({&entry, 1}, provider).Succeeded());
    const Mount mount{Root::Assets, {}, 0, 1, provider};
    MountSnapshot snapshot;
    VirtualPath path;
    VirtualFile file;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded());
    REQUIRE(path.Set(Root::Assets, "data").Succeeded());
    REQUIRE(snapshot.OpenRead(path, file).Outcome.Succeeded());
    uint8 bytes[3]{};
    AsyncReader reader;
    REQUIRE(reader.Initialize({1, 2, 8, 1, 1}) == AsyncStatus::Ok);
    RequestHandle warmup;
    REQUIRE(reader.Submit(file, 0, bytes, warmup) == AsyncStatus::Ok);
    ReadCompletion completion;
    while (reader.Poll(completion) == false)
    {
        std::this_thread::yield();
    }
    ReadTrace trace;
    REQUIRE(reader.PollTrace(trace));
    bool valid = true;
    gAllocations = 0;
    gTracking = true;
    for (uint32 index = 0; index < 100; ++index)
    {
        RequestHandle handle;
        valid &= reader.Submit(file, 0, bytes, handle, {0, 0, index, &path}) == AsyncStatus::Ok;
        valid &= reader.Cancel(handle) == AsyncStatus::Ok;
        const auto until = AsyncNow() + 5'000'000'000ULL;
        bool polled = false;
        while ((polled = reader.Poll(completion)) == false && AsyncNow() < until)
        {
            std::this_thread::yield();
        }
        valid &= polled;
        valid &= reader.PollTrace(trace);
        AsyncMetrics metrics;
        valid &= reader.Metrics(metrics) == AsyncStatus::Ok;
    }
    gTracking = false;
    REQUIRE(valid);
    REQUIRE(gAllocations == 0);
}
#endif

#if defined(LUDUS_PLATFORM_LINUX)
namespace
{
uint32 gNativeStarts = 0;
uint32 gNativeJoins = 0;
uint32 gNativeFailAt = 0;
bool gMutexFailure = false;
bool gConditionFailure = false;
} // namespace
// GNU --wrap requires exact reserved symbol spellings.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern "C" int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
extern "C" int
__wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*entry)(void*), void* context)
{
    if (++gNativeStarts == gNativeFailAt)
    {
        return EAGAIN;
    }
    return __real_pthread_create(thread, attributes, entry, context);
}
extern "C" int __wrap_pthread_join(pthread_t thread, void** output)
{
    ++gNativeJoins;
    return __real_pthread_join(thread, output);
}
extern "C" int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attributes)
{
    return gMutexFailure ? EAGAIN : __real_pthread_mutex_init(mutex, attributes);
}
extern "C" int __wrap_pthread_cond_init(pthread_cond_t* condition, const pthread_condattr_t* attributes)
{
    return gConditionFailure ? EAGAIN : __real_pthread_cond_init(condition, attributes);
}
// NOLINTEND(bugprone-reserved-identifier)
TEST_CASE("Native I/O thread and synchronization errors join partial startup exactly once")
{
    for (const uint32 failAt : {1u, 2u, 4u})
    {
        AsyncReader reader;
        gNativeStarts = 0;
        gNativeJoins = 0;
        gNativeFailAt = failAt;
        const auto status = reader.Initialize({4, 4, 16, 1, 0});
        gNativeFailAt = 0;
        REQUIRE(status == AsyncStatus::ThreadCreationFailed);
        REQUIRE(gNativeJoins == failAt - 1);
        REQUIRE(reader.Initialize({4, 2, 16, 1, 0}) == AsyncStatus::Ok);
        REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
        REQUIRE(gNativeJoins == failAt + 1);
    }
    AsyncReader reader;
    gMutexFailure = true;
    const auto mutex = reader.Initialize();
    gMutexFailure = false;
    REQUIRE(mutex == AsyncStatus::ThreadCreationFailed);
    gConditionFailure = true;
    const auto condition = reader.Initialize();
    gConditionFailure = false;
    REQUIRE(condition == AsyncStatus::ThreadCreationFailed);
    REQUIRE(reader.Initialize({1, 1, 1, 1, 0}) == AsyncStatus::Ok);
}
#endif
