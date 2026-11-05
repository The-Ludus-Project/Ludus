#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Result of resolving an `#include` path.
enum class IncludePathOutcome : foundation::uint8
{
    Ok,          ///< The normalized path was written.
    Invalid,     ///< Absolute, backslash, empty component, or a `..` that leaves the snapshot root.
    OutOfMemory, ///< Allocation failed.
    NotFound,    ///< The path is valid but names no snapshot file; set by callers that look it up.
};

/// @brief Join `path` to the directory of `includingFile` and normalize `.` and `..` components.
/// @param includingFile Snapshot path of the file containing the directive; empty to resolve
/// against the snapshot root.
/// @param path The quoted path of the directive.
/// @param outPath Receives the `/`-separated normalized path.
[[nodiscard]] IncludePathOutcome
ResolveIncludePath(std::string_view includingFile, std::string_view path, foundation::Array<char>& outPath) noexcept;

/// True when `path` is a valid snapshot path: nonempty, relative, `/`-separated, without a
/// backslash, zero byte, or empty, `.` or `..` component.
[[nodiscard]] bool IsSnapshotPath(std::string_view path) noexcept;

/// True when `text` ends with `suffix`.
[[nodiscard]] bool HasSuffix(std::string_view text, std::string_view suffix) noexcept;
} // namespace ludus::tooling::shader_language::internal
