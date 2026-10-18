#pragma once
// The world demo's checked-in random domain manifest. Each domain names one
// independent meaning of addressed randomness; IDs are assigned once and never
// recycled, and labels are tooling metadata. Contract:
// docs/architecture/randomness.md#stable-keys-domains-and-identities and
// docs/architecture/randomness.md#milestone-1-procedural-placement.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/random_domains.hpp>
namespace ludus::world_demo
{
/// Scatter positions and thinning priority.
inline constexpr foundation::uint32 RANDOM_DOMAIN_SCATTER_PLACEMENT = 1;
/// Scatter variant choice.
inline constexpr foundation::uint32 RANDOM_DOMAIN_SCATTER_VARIANT = 2;
/// Scatter rotation and scale.
inline constexpr foundation::uint32 RANDOM_DOMAIN_SCATTER_TRANSFORM = 3;
/// Regrowth of a harvested placement (milestone 1, pull request 3); reserved here so its ID never moves.
inline constexpr foundation::uint32 RANDOM_DOMAIN_SCATTER_REGROW = 4;
/// Presentation-only tint of a drawn placement; sampled during extraction and never stored or hashed.
inline constexpr foundation::uint32 RANDOM_DOMAIN_SCATTER_COSMETIC = 5;
/// The manifest, checked at compile time against the game ID range.
inline constexpr foundation::math::RandomDomain RANDOM_DOMAINS[] = {
    { .Id = RANDOM_DOMAIN_SCATTER_PLACEMENT, .Label = "scatter.placement" },
    { .Id = RANDOM_DOMAIN_SCATTER_VARIANT, .Label = "scatter.variant" },
    { .Id = RANDOM_DOMAIN_SCATTER_TRANSFORM, .Label = "scatter.transform" },
    { .Id = RANDOM_DOMAIN_SCATTER_REGROW, .Label = "scatter.regrow" },
    { .Id = RANDOM_DOMAIN_SCATTER_COSMETIC, .Label = "scatter.cosmetic" },
};
static_assert(foundation::math::CheckRandomDomains(RANDOM_DOMAINS, foundation::math::RandomDomainOwner::Game).Status ==
                  foundation::math::MathStatus::Success,
              "world demo random domains must be unique game-range IDs with labels");
} // namespace ludus::world_demo
