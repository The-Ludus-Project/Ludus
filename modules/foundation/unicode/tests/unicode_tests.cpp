#include <ludus/foundation/unicode/unicode.hpp>

#include "internal/icu_host.hpp"

#include <string_view>

#include <catch2/catch_test_macros.hpp>

// Host lifecycle of the pinned ICU4C services. Allocation-failure fail-stop runs
// in separate processes (modules/localization/tests/format_probe.cpp), because
// a denied ICU allocation terminates by design.

using namespace ludus::foundation;
using namespace ludus::foundation::unicode;

namespace
{
constexpr std::string_view PINNED_LABEL = "icu4c-78.3/cldr-48/unicode-17.0";
} // namespace

TEST_CASE("Unicode host reports the pinned data and releases every allocation at teardown")
{
    REQUIRE(IsUnicodeHostInitialized() == false);
    REQUIRE(GetLinguisticDataLabel().empty() == true);
    REQUIRE(ShutdownUnicodeHost() == UnicodeStatus::NotInitialized);
    REQUIRE(detail::TryRetainHost() == false);

    REQUIRE(InitializeUnicodeHost(GetSystemAllocationDomain()) == UnicodeStatus::Ok);
    CHECK(IsUnicodeHostInitialized() == true);
    CHECK(GetLinguisticDataLabel() == PINNED_LABEL);
    CHECK(InitializeUnicodeHost(GetSystemAllocationDomain()) == UnicodeStatus::AlreadyInitialized);
    const UnicodeAllocationStatistics warmed = GetUnicodeAllocationStatistics();
    // Warm-up created the static locale cache, UnifiedCache and skeleton tables.
    CHECK(warmed.Allocations > 0);
    CHECK(warmed.LiveBlocks > 0);
    CHECK(warmed.PeakBytes >= warmed.LiveBytes);

    icu::Locale locale("und");
    CHECK(detail::MakeLocale("ar", locale) == U_ZERO_ERROR);
    CHECK(std::string_view(locale.getLanguage()) == "ar");
    CHECK(detail::MakeLocale("fr-FR", locale) == U_ZERO_ERROR);
    CHECK(std::string_view(locale.getCountry()) == "FR");
    const std::string_view tooLong(
        "abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-"
        "abcdefgh-abcdefgh-abcdefgh-abcdefgh");
    CHECK(detail::MakeLocale(tooLong, locale) == U_ILLEGAL_ARGUMENT_ERROR);
    CHECK(std::string_view(locale.getCountry()) == "FR");
    CHECK(detail::MakeUnicodeString("\u00e9") == icu::UnicodeString(u"\u00e9"));

    REQUIRE(detail::TryRetainHost() == true);
    detail::ReleaseHost();
    REQUIRE(ShutdownUnicodeHost() == UnicodeStatus::Ok);
    CHECK(IsUnicodeHostInitialized() == false);
    CHECK(GetLinguisticDataLabel().empty() == true);
    const UnicodeAllocationStatistics released = GetUnicodeAllocationStatistics();
    CHECK(released.LiveBlocks == 0);
    CHECK(released.LiveBytes == 0);
    CHECK(released.Frees == released.Allocations);

    // u_cleanup clears hooks and data, so a later host starts again from scratch.
    REQUIRE(InitializeUnicodeHost(GetSystemAllocationDomain()) == UnicodeStatus::Ok);
    CHECK(GetLinguisticDataLabel() == PINNED_LABEL);
    REQUIRE(ShutdownUnicodeHost() == UnicodeStatus::Ok);
    CHECK(GetUnicodeAllocationStatistics().LiveBlocks == 0);
}
