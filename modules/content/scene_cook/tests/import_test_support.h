#pragma once

// Shared helpers for the glTF/OBJ/image importer tests: byte views, scratch
// directories, fixture paths and file operations. Test-only code (exceptions
// enabled): failure is a Catch2 assertion. Paths are UTF-8 strings joined with
// `/`; the operations come from ludus_test_files, so this header does not
// parse <filesystem> (docs/development/build-profiling.md).

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/strings/string.hpp>

#include "scratch_directory.h"
#include "test_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <span>
#include <string>
#include <string_view>

namespace ludus::scene_import_tests
{
using foundation::core::uint64;
using foundation::core::uint8;
using scene_cook_tests::JoinPath;
using test_support::IsDirectory;
using test_support::IsExistingPath;

inline std::span<const uint8> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const uint8*>(text.data()), text.size()};
}

/// Whole file at `path`; the test fails when it cannot be read.
inline std::string ReadFileText(std::string_view path)
{
    foundation::String bytes;
    REQUIRE(test_support::ReadFile(path, bytes) == true);
    return std::string(bytes.GetView());
}

/// Replace the file at `path`, creating missing parent directories.
inline void WriteFile(std::string_view path, std::string_view data)
{
    REQUIRE(test_support::WriteFile(path, AsBytes(data)) == true);
}

/// Create `path` and any missing parents.
inline void CreateDirectories(std::string_view path)
{
    REQUIRE(test_support::CreateDirectories(path) == true);
}

/// Copy the regular file `source` to the new file `destination`.
inline void CopyRegularFile(std::string_view source, std::string_view destination)
{
    REQUIRE(test_support::CopyRegularFile(source, destination) == true);
}

/// Size in bytes of the regular file at `path`.
inline uint64 GetFileSize(std::string_view path)
{
    uint64 size = 0;
    REQUIRE(test_support::GetFileSize(path, size) == true);
    return size;
}

/// Absolute path of `path` without links, spelled with `/` separators.
inline std::string GetCanonicalPath(std::string_view path)
{
    foundation::String canonical;
    REQUIRE(test_support::GetCanonicalPath(path, canonical) == true);
    return std::string(canonical.GetView());
}

/// Checked-in fixture below tests/fixtures.
inline std::string GetFixture(std::string_view relative)
{
    return JoinPath(LUDUS_SCENE_IMPORT_FIXTURES_DIR, relative);
}

/// Fresh scratch directory removed on destruction.
struct ScratchDirectory final
{
    std::string Path;
    ScratchDirectory()
    {
        Path = scene_cook_tests::MakeScratchDirectory("ludus-scene-import");
    }
    ~ScratchDirectory()
    {
        static_cast<void>(test_support::RemoveTree(Path));
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    /// `relative` below the scratch directory.
    [[nodiscard]] std::string GetPath(std::string_view relative) const
    {
        return JoinPath(Path, relative);
    }
};
} // namespace ludus::scene_import_tests
