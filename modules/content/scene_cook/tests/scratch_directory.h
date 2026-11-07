#pragma once

// Portable fresh scratch directory for the scene cooker tests (Linux, macOS and
// Windows). Test-only code: failure throws, which Catch2 reports.

#include <ludus/foundation/base/types.h>

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ludus::scene_cook_tests
{
/// Create a new, empty directory named prefix-<random> in the system temporary directory.
inline std::filesystem::path MakeScratchDirectory(std::string_view prefix)
{
    constexpr foundation::core::uint32 MAX_ATTEMPTS = 64;
    std::random_device entropy;
    const std::filesystem::path parent = std::filesystem::temp_directory_path();
    for (foundation::core::uint32 attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
    {
        const foundation::core::uint32 suffix = entropy();
        const std::filesystem::path candidate = parent / (std::string(prefix) + "-" + std::to_string(suffix));
        if (std::filesystem::create_directory(candidate) == true)
        {
            return candidate;
        }
    }
    throw std::runtime_error("could not create a fresh scratch directory");
}
} // namespace ludus::scene_cook_tests
