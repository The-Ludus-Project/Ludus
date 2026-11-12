#pragma once

// Portable fresh scratch directory for the scene cooker tests (Linux, macOS and
// Windows). Test-only code: failure is a Catch2 assertion. Host file operations
// go through ludus_test_files, so this header does not parse <filesystem>
// (docs/development/build-profiling.md).

#include <ludus/foundation/strings/string.hpp>

#include "test_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

namespace ludus::scene_cook_tests
{
/// Create a new, empty directory named prefix-<random> in the system temporary directory.
inline std::string MakeScratchDirectory(std::string_view prefix)
{
    foundation::String path;
    REQUIRE(test_support::CreateScratchDirectory(prefix, path) == true);
    return std::string(path.GetView());
}

/// `base` followed by a `/` separator and `relative`; `/` is accepted on every host.
inline std::string JoinPath(std::string_view base, std::string_view relative)
{
    std::string path(base);
    path.push_back('/');
    path.append(relative);
    return path;
}
} // namespace ludus::scene_cook_tests
