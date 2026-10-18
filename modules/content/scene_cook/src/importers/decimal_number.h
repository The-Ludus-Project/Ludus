#pragma once

// Locale-free decimal text conversion shared by the best-effort text readers
// (pbrt-v4, PLY and PFM headers, Wavefront OBJ/MTL). Conversion uses the
// bounded FoundationBase ParseFloat32 backend; no standard-library parsing.

#include <ludus/foundation/base/core.h>

#include <string_view>

namespace ludus::content::scene_cook::importers
{
using foundation::core::float32;
using foundation::core::float64;
using foundation::core::usize;

/// Longest admitted decimal literal in bytes.
inline constexpr usize MAX_DECIMAL_TEXT_BYTES = 4096;

/// Locale-free conversion of a decimal literal (optional sign, digits,
/// optional fraction and exponent; leading '.' and trailing '.' are accepted).
/// Rounds to binary32 (correctly rounded), then widens; underflow yields a signed zero.
/// @return false for malformed, overlong or overflowing input; outValue is unchanged then.
[[nodiscard]] bool ParseNumber(std::string_view text, float64& outValue) noexcept;
} // namespace ludus::content::scene_cook::importers
