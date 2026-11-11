#include <ludus/foundation/identity/lifetime.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include "internal/lifetime_source.hpp"

#include <array>
#include <atomic>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::identity;
namespace threading = ludus::foundation::threading;

static_assert(noexcept(TryIssueLifetimeId(std::declval<uint64&>())));

TEST_CASE("process lifetime tokens are nonzero and strictly increasing", "[identity][lifetime]")
{
    uint64 previous = 0;
    for (uint32 i = 0; i < 64; ++i)
    {
        uint64 token = 0;
        REQUIRE(TryIssueLifetimeId(token) == LifetimeIdStatus::Success);
        CHECK(token != 0);
        CHECK(token > previous);
        previous = token;
    }
}

TEST_CASE("the lifetime source starts at one and never wraps", "[identity][lifetime]")
{
    std::atomic<uint64> next{1};
    uint64 token = 99;
    REQUIRE(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::Success);
    CHECK(token == 1);

    next.store(LAST_LIFETIME_ID - 1);
    REQUIRE(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::Success);
    CHECK(token == LAST_LIFETIME_ID - 1);
    REQUIRE(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::Success);
    CHECK(token == LAST_LIFETIME_ID);

    // Terminal: exhaustion is sticky and leaves the output unchanged.
    CHECK(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::IdentityExhausted);
    CHECK(token == LAST_LIFETIME_ID);
    CHECK(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::IdentityExhausted);
    CHECK(token == LAST_LIFETIME_ID);
    CHECK(next.load() == LAST_LIFETIME_ID + 1);
}

TEST_CASE("a zero counter is exhausted rather than issuing the null owner", "[identity][lifetime]")
{
    std::atomic<uint64> next{0};
    uint64 token = 5;
    CHECK(internal::TryIssueLifetimeIdFrom(next, token) == LifetimeIdStatus::IdentityExhausted);
    CHECK(token == 5);
}

TEST_CASE("concurrent issuance never repeats a token", "[identity][lifetime]")
{
    constexpr usize THREAD_COUNT = 4;
    constexpr usize TOKENS_PER_THREAD = 2048;
    constexpr usize TOKEN_COUNT = THREAD_COUNT * TOKENS_PER_THREAD;
    std::atomic<uint64> next{1};
    static std::array<std::array<uint64, TOKENS_PER_THREAD>, THREAD_COUNT> issued{};
    std::atomic<bool> hasFailed{false};
    std::array<threading::Thread, THREAD_COUNT> threads;
    for (usize t = 0; t < THREAD_COUNT; ++t)
    {
        // Workers report through an atomic; Catch assertions stay on the test thread.
        REQUIRE(threads[t].Start([&next, &hasFailed, &tokens = issued[t]] {
            for (uint64& token : tokens)
            {
                if (internal::TryIssueLifetimeIdFrom(next, token) != LifetimeIdStatus::Success)
                {
                    hasFailed.store(true);
                    return;
                }
            }
        }) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    REQUIRE(hasFailed.load() == false);
    // Tokens are 1..TOKEN_COUNT; each must be issued exactly once.
    static std::array<bool, TOKEN_COUNT + 1> seen{};
    for (const std::array<uint64, TOKENS_PER_THREAD>& tokens : issued)
    {
        uint64 previous = 0;
        for (const uint64 token : tokens)
        {
            REQUIRE(token >= 1);
            REQUIRE(token <= TOKEN_COUNT);
            const usize index = static_cast<usize>(token);
            CHECK(seen[index] == false);
            seen[index] = true;
            // Each thread observes its own tokens in increasing order.
            CHECK(token > previous);
            previous = token;
        }
    }
    CHECK(next.load() == TOKEN_COUNT + 1);
}
