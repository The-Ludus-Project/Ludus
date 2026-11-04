#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Large-file, very-large-file and long-line thresholds for the file document
/// host (CE22). Each threshold has exactly one owner constant here; every read,
/// admission and degradation decision in the file host and its views reads these
/// values and never a bare literal. The byte values follow the architecture's
/// "Large files and long lines" table (docs/architecture/code-editor.md).
///
/// The modes degrade as follows, with a banner explaining the active mode:
/// - Normal: up to CODE_FILE_NORMAL_LIMIT_BYTES and CODE_FILE_NORMAL_LIMIT_LINES.
///   Every feature is on.
/// - Large: up to CODE_FILE_LARGE_LIMIT_BYTES. Structural features are off and
///   coloring is visible-block only; folding falls back to indentation.
/// - Very large: up to CODE_FILE_READ_ONLY_LIMIT_BYTES. The file opens read-only
///   after confirmation.
/// - Beyond that: the host refuses the open and offers Open Externally.
///
/// A line longer than CODE_LONG_LINE_UTF16_UNITS gets no formats, no bracket
/// matching and no inlay hints, because Qt still lays out the whole block, which
/// the M0 baseline measured as the dominant per-keystroke cost on a 1 MiB line.
enum class CodeFileSizeMode : foundation::uint8
{
    /// Within the normal byte and line limits; every feature is available.
    Normal,
    /// Above the normal limits but within the large-file limit; features degrade.
    Large,
    /// Above the large-file limit but within the read-only limit; opens read-only.
    VeryLarge,
    /// Above the read-only limit; the host refuses the open.
    Refused,
};

/// A megabyte in bytes, so the thresholds below read in their documented units.
inline constexpr foundation::usize CODE_FILE_BYTES_PER_MIB = foundation::usize{1024} * foundation::usize{1024};

/// Upper byte size of a normal document: every feature is on at or below it.
inline constexpr foundation::usize CODE_FILE_NORMAL_LIMIT_BYTES = foundation::usize{8} * CODE_FILE_BYTES_PER_MIB;
/// Upper line count of a normal document; above it the document degrades to Large.
inline constexpr foundation::usize CODE_FILE_NORMAL_LIMIT_LINES = 200000;
/// Upper byte size of a large document; structural features are off in this band.
inline constexpr foundation::usize CODE_FILE_LARGE_LIMIT_BYTES = foundation::usize{64} * CODE_FILE_BYTES_PER_MIB;
/// Upper byte size that still opens, read-only after confirmation; refused beyond it.
inline constexpr foundation::usize CODE_FILE_READ_ONLY_LIMIT_BYTES = foundation::usize{256} * CODE_FILE_BYTES_PER_MIB;
/// A line wider than this many UTF-16 units loses per-line formats and matching.
inline constexpr foundation::usize CODE_LONG_LINE_UTF16_UNITS = 20000;

/// Classify a byte count against the size thresholds. Line-count degradation
/// (CODE_FILE_NORMAL_LIMIT_LINES) is applied by the caller, which knows the
/// decoded line count; this function sees only bytes.
[[nodiscard]] constexpr CodeFileSizeMode ClassifyCodeFileSize(foundation::usize byteCount) noexcept
{
    if (byteCount > CODE_FILE_READ_ONLY_LIMIT_BYTES)
    {
        return CodeFileSizeMode::Refused;
    }
    if (byteCount > CODE_FILE_LARGE_LIMIT_BYTES)
    {
        return CodeFileSizeMode::VeryLarge;
    }
    if (byteCount > CODE_FILE_NORMAL_LIMIT_BYTES)
    {
        return CodeFileSizeMode::Large;
    }
    return CodeFileSizeMode::Normal;
}
} // namespace ludus::editor
