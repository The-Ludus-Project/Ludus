#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace ludus::foundation;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;

static_assert(std::is_copy_constructible_v<threading::Thread> == false);
static_assert(std::is_move_constructible_v<threading::Thread> == false);
static_assert(std::is_nothrow_default_constructible_v<threading::Thread>);

namespace
{
constexpr usize POOL_THREAD_COUNT = 4;
constexpr uint64 SLEEP_MILLISECONDS = 5;

struct CounterContext final
{
    std::atomic<uint32> Calls{0};
    threading::ThreadId Worker;
};

void IncrementCounter(void* contextOrNull) noexcept
{
    CounterContext& context = *static_cast<CounterContext*>(contextOrNull);
    context.Worker = threading::GetCallingThreadId();
    context.Calls.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

#if defined(LUDUS_PLATFORM_WEB)
TEST_CASE("browser builds report threads as unavailable", "[threading][thread]")
{
    CounterContext context;
    threading::Thread thread;
    CHECK(thread.Start(IncrementCounter, &context) == threading::ThreadStatus::Unavailable);
    CHECK(thread.IsJoinable() == false);
    CHECK(thread.Join() == threading::ThreadStatus::InvalidState);
    CHECK(context.Calls.load() == 0);
    threading::SleepFor(time::Milliseconds(SLEEP_MILLISECONDS));
    threading::YieldThread();
    CHECK(threading::GetCallingThreadId() == threading::GetCallingThreadId());
}
#else
TEST_CASE("a thread runs its entry once and reports its lifecycle", "[threading][thread]")
{
    CounterContext context;
    threading::Thread thread;
    CHECK(thread.IsJoinable() == false);
    CHECK(thread.Join() == threading::ThreadStatus::InvalidState);
    CHECK(thread.Start(nullptr, &context) == threading::ThreadStatus::InvalidArgument);
    CHECK(thread.IsJoinable() == false);

    REQUIRE(thread.Start(IncrementCounter, &context) == threading::ThreadStatus::Ok);
    CHECK(thread.IsJoinable() == true);
    CHECK(thread.Start(IncrementCounter, &context) == threading::ThreadStatus::InvalidState);
    REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    CHECK(thread.IsJoinable() == false);
    CHECK(context.Calls.load() == 1);
    CHECK((context.Worker == threading::GetCallingThreadId()) == false);
    CHECK(threading::GetCallingThreadId() == threading::GetCallingThreadId());

    // The object is reusable after Join.
    REQUIRE(thread.Start(IncrementCounter, &context) == threading::ThreadStatus::Ok);
    REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    CHECK(context.Calls.load() == 2);
}

TEST_CASE("a thread invokes a caller-owned callable", "[threading][thread]")
{
    std::atomic<uint32> sum{0};
    auto addOne = [&sum]() { sum.fetch_add(1, std::memory_order_relaxed); };
    StaticArray<threading::Thread, POOL_THREAD_COUNT> pool;
    for (threading::Thread& thread : pool)
    {
        REQUIRE(thread.Start(addOne) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : pool)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    CHECK(sum.load() == POOL_THREAD_COUNT);
}

TEST_CASE("a thread owns a callable passed by value until it is joined", "[threading][thread]")
{
    struct Tally final
    {
        std::atomic<uint32>* CallsOrNull = nullptr;
        std::atomic<uint32>* DestroyedOrNull = nullptr;
        Tally(std::atomic<uint32>& inoutCalls, std::atomic<uint32>& inoutDestroyed) noexcept
            : CallsOrNull(&inoutCalls), DestroyedOrNull(&inoutDestroyed)
        {
        }
        Tally(Tally&& other) noexcept : CallsOrNull(other.CallsOrNull), DestroyedOrNull(other.DestroyedOrNull)
        {
            other.CallsOrNull = nullptr;
            other.DestroyedOrNull = nullptr;
        }
        Tally(const Tally&) = delete;
        Tally& operator=(const Tally&) = delete;
        Tally& operator=(Tally&&) = delete;
        ~Tally() noexcept
        {
            if (DestroyedOrNull != nullptr)
            {
                DestroyedOrNull->fetch_add(1, std::memory_order_relaxed);
            }
        }
        void operator()() noexcept
        {
            CallsOrNull->fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::atomic<uint32> calls{0};
    std::atomic<uint32> destroyed{0};
    threading::Thread thread;
    REQUIRE(thread.Start(Tally{calls, destroyed}) == threading::ThreadStatus::Ok);
    // A rejected start stores nothing; the caller's temporary is the caller's to destroy.
    CHECK(thread.Start([]() {}) == threading::ThreadStatus::InvalidState);
    CHECK(destroyed.load() == 0);
    REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    CHECK(calls.load() == 1);
    // The moved-from temporary destroys without counting; the stored copy counts once at Join.
    CHECK(destroyed.load() == 1);

    uint32 plain = 0;
    REQUIRE(thread.Start([&plain]() { ++plain; }) == threading::ThreadStatus::Ok);
    REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    CHECK(plain == 1);
}

TEST_CASE("sleeping advances the monotonic clock by at least the interval", "[threading][thread]")
{
    const time::Timestamp start = time::Now();
    threading::SleepFor(time::Milliseconds(SLEEP_MILLISECONDS));
    threading::YieldThread();
    time::Duration elapsed;
    REQUIRE(time::TryElapsed(start, time::Now(), elapsed) == time::TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds >= time::Milliseconds(SLEEP_MILLISECONDS).Nanoseconds);
    threading::SleepFor(time::Duration{});
}
#endif
