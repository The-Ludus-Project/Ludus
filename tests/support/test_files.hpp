#pragma once

// Test-only host filesystem operations for Catch2 fixtures (ludus_test_files).
//
// <filesystem> costs about 430 ms of frontend time in every translation unit that
// parses it (docs/development/build-profiling.md, "Filesystem in test fixtures").
// Fixtures that need scratch directories, file copies or whole-file reads call
// these operations instead, so <filesystem> is parsed once, in test_files.cpp.
// Production code must use FoundationFilesystem; this library is linked only by
// test targets and is not installed.
//
// Paths are UTF-8 text in the host spelling. Every operation reports failure
// through its return value and is exception-free; output parameters are left
// unchanged when an operation fails.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/strings/string.hpp>

#include <span>
#include <string_view>

namespace ludus::test_support
{
using foundation::uint64;
using foundation::uint8;

/// One entry reported by VisitDirectory; the views are valid only during the callback.
struct DirectoryEntry final
{
    /// Full path of the entry: the visited directory joined with its relative location.
    std::string_view Path;
    /// Final path component of the entry.
    std::string_view Name;
    /// True when the entry is a regular file (following symbolic links).
    bool IsRegularFile = false;
    /// True when the entry is a directory (following symbolic links).
    bool IsDirectory = false;
};

/// Whether VisitDirectory descends into subdirectories.
enum class DirectoryWalk : uint8
{
    /// Report only the directory's immediate entries.
    Immediate,
    /// Report every entry below the directory, without following directory symbolic links.
    Recursive,
};

/// Callback invoked once per entry with the `context` pointer passed to VisitDirectory.
using DirectoryEntryVisitor = void (*)(const DirectoryEntry& entry, void* context) noexcept;

/// Creates a new, empty directory named `<prefix>-<random hex>` in the system temporary directory.
/// @param prefix File-name prefix; it must not contain separators.
/// @param outPath Receives the new directory's absolute path.
/// @return False when no fresh directory could be created after a bounded number of attempts.
[[nodiscard]] bool CreateScratchDirectory(std::string_view prefix, foundation::String& outPath) noexcept;

/// Creates `path` and any missing parents.
/// @return True when `path` exists as a directory afterwards.
[[nodiscard]] bool CreateDirectories(std::string_view path) noexcept;

/// Removes `path` and everything below it. A missing `path` is not a failure.
/// @return True when `path` no longer exists.
[[nodiscard]] bool RemoveTree(std::string_view path) noexcept;

/// Copies the directory tree `source` to `destination`, which must not exist yet.
/// @return False when any directory or file could not be copied.
[[nodiscard]] bool CopyTree(std::string_view source, std::string_view destination) noexcept;

/// Copies the regular file `source` to `destination`, which must not exist yet.
[[nodiscard]] bool CopyRegularFile(std::string_view source, std::string_view destination) noexcept;

/// Removes the file, symbolic link or empty directory `path`.
/// @return False when `path` did not exist or could not be removed.
[[nodiscard]] bool RemoveFile(std::string_view path) noexcept;

/// Reads the whole file at `path` as bytes.
/// @param outBytes Receives the file's bytes; it may hold zero bytes and keeps its allocation domain.
[[nodiscard]] bool ReadFile(std::string_view path, foundation::String& outBytes) noexcept;

/// Replaces the file at `path` with `bytes`, first creating any missing parent directories.
[[nodiscard]] bool WriteFile(std::string_view path, std::span<const uint8> bytes) noexcept;

/// True when `path` names an existing filesystem object (following symbolic links).
[[nodiscard]] bool IsExistingPath(std::string_view path) noexcept;

/// True when `path` names an existing directory (following symbolic links).
[[nodiscard]] bool IsDirectory(std::string_view path) noexcept;

/// Reads the size of the regular file at `path`.
/// @param outBytes Receives the size in bytes.
[[nodiscard]] bool GetFileSize(std::string_view path, uint64& outBytes) noexcept;

/// Resolves `path` to an absolute path without symbolic links, `.` or `..` components.
/// @param outPath Receives the path spelled with `/` separators on every host.
[[nodiscard]] bool GetCanonicalPath(std::string_view path, foundation::String& outPath) noexcept;

/// Calls `visitor` for every entry below `directory`, in unspecified order.
/// @param visitor Callback; never null.
/// @param context Borrowed pointer passed unchanged to every `visitor` call; never null.
/// @return False when `directory` could not be opened or a listing step failed; entries reported
/// before the failure stay reported.
[[nodiscard]] bool
VisitDirectory(std::string_view directory, DirectoryWalk walk, DirectoryEntryVisitor visitor, void* context) noexcept;
} // namespace ludus::test_support
