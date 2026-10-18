// Domain manifest checks: ID ranges, uniqueness and labels, in constant and runtime evaluation.
#include <ludus/foundation/math/random_domains.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::math;

namespace
{
constexpr RandomDomain GAME_DOMAINS[] = {
    {1u, "first"},
    {2u, "second"},
    {RANDOM_GAME_DOMAIN_LAST, "last"},
};
static_assert(CheckRandomDomains(GAME_DOMAINS, RandomDomainOwner::Game).Status == MathStatus::Success);

constexpr RandomDomain ENGINE_DOMAINS[] = {
    {0x42414B45u, "renderer bake"},
    {0x52543031u, "renderer reference"},
    {RANDOM_ENGINE_DOMAIN_FIRST, "first engine"},
};
static_assert(CheckRandomDomains(ENGINE_DOMAINS, RandomDomainOwner::Engine).Status == MathStatus::Success);
} // namespace

TEST_CASE("Domain ranges separate game and engine manifests", "[math][random]")
{
    CHECK(IsRandomDomainInRange(0u, RandomDomainOwner::Game) == false);
    CHECK(IsRandomDomainInRange(0u, RandomDomainOwner::Engine) == false);
    CHECK(IsRandomDomainInRange(RANDOM_GAME_DOMAIN_LAST, RandomDomainOwner::Game) == true);
    CHECK(IsRandomDomainInRange(RANDOM_GAME_DOMAIN_LAST, RandomDomainOwner::Engine) == false);
    CHECK(IsRandomDomainInRange(RANDOM_ENGINE_DOMAIN_FIRST, RandomDomainOwner::Game) == false);
    CHECK(IsRandomDomainInRange(RANDOM_ENGINE_DOMAIN_FIRST, RandomDomainOwner::Engine) == true);
    CHECK(IsRandomDomainInRange(0xffffffffu, RandomDomainOwner::Engine) == true);
    CHECK(RANDOM_GAME_DOMAIN_LAST + 1u == RANDOM_ENGINE_DOMAIN_FIRST);
}

TEST_CASE("Domain manifests reject zero, range, label and duplicate errors", "[math][random]")
{
    const RandomDomain zero[] = {{1u, "a"}, {0u, "zero"}};
    const RandomDomainCheck zeroCheck = CheckRandomDomains(zero, RandomDomainOwner::Game);
    CHECK(zeroCheck.Status == MathStatus::InvalidArgument);
    CHECK(zeroCheck.Index == 1);

    const RandomDomain engineInGame[] = {{0x42414B45u, "bake"}};
    CHECK(CheckRandomDomains(engineInGame, RandomDomainOwner::Game).Status == MathStatus::InvalidArgument);

    const RandomDomain gameInEngine[] = {{0x42414B45u, "bake"}, {7u, "game"}};
    const RandomDomainCheck gameCheck = CheckRandomDomains(gameInEngine, RandomDomainOwner::Engine);
    CHECK(gameCheck.Status == MathStatus::InvalidArgument);
    CHECK(gameCheck.Index == 1);

    const RandomDomain unlabelled[] = {{3u, nullptr}};
    CHECK(CheckRandomDomains(unlabelled, RandomDomainOwner::Game).Status == MathStatus::InvalidArgument);

    // The later entry of a duplicate pair is reported, regardless of labels.
    const RandomDomain duplicate[] = {{5u, "loot"}, {6u, "spread"}, {5u, "renamed loot"}};
    const RandomDomainCheck duplicateCheck = CheckRandomDomains(duplicate, RandomDomainOwner::Game);
    CHECK(duplicateCheck.Status == MathStatus::InvalidArgument);
    CHECK(duplicateCheck.Index == 2);

    const RandomDomain valid[] = {{9u, "terrain"}, {4u, "ai"}};
    const RandomDomainCheck validCheck = CheckRandomDomains(valid, RandomDomainOwner::Game);
    CHECK(validCheck.Status == MathStatus::Success);
    CHECK(validCheck.Index == 0);
}
