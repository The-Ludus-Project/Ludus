#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>

// Golden weather route shared by ludus_gameplay_weather_tests, tools/web-weather-probe, the Editor tests and the
// installed SDK consumer, so every build checks one digest. Exception-free; Catch2 stays in the tests.
namespace ludus::gameplay::weather::testing
{
/// Digest of the golden route, recorded from the native reference build. A change to resolution rules,
/// codes or the corpus itself changes it; update it only with an explained behavior change.
inline constexpr foundation::Fingerprint128 EXPECTED_WEATHER_GOLDEN_DIGEST
{
    .Low = 0xecc6a6256703d0a2ULL,
    .High = 0x578b8f944f01617cULL,
};

/// Fixed tick rate of the corpus climate: Play's PLAYABLE_WORLD_TICKS_PER_SECOND, so a World climate can cook to it.
inline constexpr foundation::uint32 GOLDEN_TICKS_PER_SECOND = 60;
/// Ticks the golden route runs.
inline constexpr foundation::uint32 GOLDEN_ROUTE_TICKS = 200;

/// Cook the corpus climate: four presets, a default schedule, a feathered valley region and an overlapping
/// higher-priority ridge region.
/// @param isReordered Author presets, schedules and regions in reverse order; the bytes must not change.
/// @param presetRevision Revision of every preset, to model an incompatible preset edit.
/// @param outBytes Receives the blob.
bool TryCookGoldenClimate(bool isReordered,
                          foundation::uint32 presetRevision,
                          foundation::Array<foundation::uint8>& outBytes) noexcept;

/// Run every golden route (single ticks, chunked ticks, a save/load split and reordered authoring and
/// submission) and write the shared digest when all of them agree.
bool TryComputeWeatherGoldenDigest(foundation::Fingerprint128& outDigest) noexcept;

/// Whether every route agrees and the digest equals EXPECTED_WEATHER_GOLDEN_DIGEST.
bool RunWeatherGoldenCorpus() noexcept;
} // namespace ludus::gameplay::weather::testing
