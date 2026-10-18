#pragma once

// Compile-time checks for checked-in random domain manifests. A domain ID names
// one independent meaning of addressed randomness (TryMakeRandomKey); game and
// engine owners assign IDs from separate ranges so neither needs a shared
// registry. See docs/architecture/randomness.md#stable-keys-domains-and-identities.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>

namespace ludus::foundation::math
{
using core::uint32;
using core::usize;

/// First domain ID a game or content manifest may assign.
inline constexpr uint32 RANDOM_GAME_DOMAIN_FIRST = 1u;
/// Last domain ID a game or content manifest may assign.
inline constexpr uint32 RANDOM_GAME_DOMAIN_LAST = 0x00ffffffu;
/// First engine-owned domain ID; engine IDs are conventionally four ASCII characters such as `BAKE`.
inline constexpr uint32 RANDOM_ENGINE_DOMAIN_FIRST = 0x01000000u;

/// @brief Which ID range a manifest draws from.
enum class RandomDomainOwner : core::uint8
{
    /// Game or content manifests: RANDOM_GAME_DOMAIN_FIRST through RANDOM_GAME_DOMAIN_LAST.
    Game,
    /// Engine modules: RANDOM_ENGINE_DOMAIN_FIRST through the largest uint32.
    Engine,
};

/// @brief One entry of a checked-in manifest.
///
/// The ID is persistent: it is assigned once and never recycled. The label is tooling metadata, so renaming it
/// does not change any derived key.
struct RandomDomain final
{
    /// Persistent nonzero domain ID passed to TryMakeRandomKey.
    uint32 Id = 0;
    /// Human-readable name for tools and diagnostics; must not be null.
    const char* Label = "";
};

/// @brief Outcome of CheckRandomDomains.
struct RandomDomainCheck final
{
    /// Success, or InvalidArgument for the first rejected entry.
    MathStatus Status = MathStatus::Success;
    /// Index of the first rejected entry; zero on success.
    usize Index = 0;
};

/// @brief Whether a domain ID lies in the owner's range.
[[nodiscard]] constexpr bool IsRandomDomainInRange(uint32 id, RandomDomainOwner owner) noexcept
{
    if (owner == RandomDomainOwner::Game)
    {
        return id >= RANDOM_GAME_DOMAIN_FIRST && id <= RANDOM_GAME_DOMAIN_LAST;
    }
    return id >= RANDOM_ENGINE_DOMAIN_FIRST;
}

/// @brief Check a manifest: every ID in the owner's range, unique, and every label present.
///
/// Usable in constant expressions, so a checked-in manifest can assert itself with
/// `static_assert(CheckRandomDomains(DOMAINS, RandomDomainOwner::Game).Status == MathStatus::Success)`.
/// The comparison is quadratic in the entry count, which suits hand-maintained manifests.
/// @param domains The manifest entries, in any order.
/// @param owner The range every entry must use.
/// @return Success, or InvalidArgument with the index of the first entry that is out of range, has a null label
///         or repeats an earlier ID.
template <usize Count>
[[nodiscard]] constexpr RandomDomainCheck CheckRandomDomains(const RandomDomain (&domains)[Count],
                                                             RandomDomainOwner owner) noexcept
{
    for (usize i = 0; i < Count; ++i)
    {
        if (IsRandomDomainInRange(domains[i].Id, owner) == false || domains[i].Label == nullptr)
        {
            return {MathStatus::InvalidArgument, i};
        }
        for (usize j = 0; j < i; ++j)
        {
            if (domains[j].Id == domains[i].Id)
            {
                return {MathStatus::InvalidArgument, i};
            }
        }
    }
    return {};
}
} // namespace ludus::foundation::math
