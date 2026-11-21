#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>

// Golden weather route shared by ludus_gameplay_weather_tests and tools/web-weather-probe, so native and
// browser builds check one digest. The corpus is exception-free engine-style code; Catch2 stays in the tests.
namespace ludus::gameplay::weather::testing
{
/// Digest of the golden route, recorded from the native reference build. A change to resolution rules,
/// codes or the corpus itself changes it; update it only with an explained behavior change.
inline constexpr foundation::Fingerprint128 EXPECTED_WEATHER_GOLDEN_DIGEST
{
    .Low = 0x45af067cf554d26dULL,
    .High = 0xcc60b97be44e1126ULL,
};

/// Fixed tick rate of the corpus climate.
inline constexpr foundation::uint32 GOLDEN_TICKS_PER_SECOND = 30;
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
