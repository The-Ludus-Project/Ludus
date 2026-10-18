#pragma once

// Private ICU-facing helpers of Ludus::Unicode for engine owners compiled in the
// same build (Localization's formatter backend). Never installed: ICU types
// must not reach a public engine header or the SDK include set.

#include <ludus/foundation/base/types.h>

#include <string_view>

#include <unicode/locid.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>

namespace ludus::foundation::unicode::detail
{
/// Largest locale label the host converts, matching the catalog label limit.
inline constexpr usize MAX_LOCALE_TAG_BYTES = 128;

/// @brief Registers one ICU user that must be destroyed before ShutdownUnicodeHost.
/// @return false, with nothing registered, when the host is not running.
[[nodiscard]] bool TryRetainHost() noexcept;

/// Releases a registration made by a successful TryRetainHost.
void ReleaseHost() noexcept;

/// @brief Builds a locale from an explicit BCP 47 tag; never reads the process default.
///
/// The default `icu::Locale` constructor and static getters are not safe under
/// allocation failure, so every locale comes from an explicit tag.
/// @return U_ZERO_ERROR with outLocale written; otherwise outLocale is unchanged.
[[nodiscard]] UErrorCode MakeLocale(std::string_view tag, icu::Locale& outLocale) noexcept;

/// @brief Converts validated UTF-8; ICU would silently replace invalid bytes with U+FFFD.
/// @pre utf8 is valid UTF-8 within INT32_MAX bytes.
[[nodiscard]] icu::UnicodeString MakeUnicodeString(std::string_view utf8) noexcept;

/// @brief Test hook: treat the Nth ICU allocation attempt from now (1-based) as a failure.
///
/// Zero disables the fail point. A denied attempt takes the production
/// fail-stop path and terminates, so tests run it in a separate process.
void SetAllocationFailPoint(uint64 attempt) noexcept;
} // namespace ludus::foundation::unicode::detail
