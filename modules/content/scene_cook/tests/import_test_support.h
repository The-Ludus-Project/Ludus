#pragma once

// Shared helpers for the glTF/OBJ/image importer tests: byte views, scratch
// directories and fixture paths. Test-only code (exceptions enabled).

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <unistd.h> // mkdtemp (POSIX; <stdlib.h> alone does not declare it under strict C++23 on Darwin).

#include <ludus/foundation/base/types.h>

namespace ludus::scene_import_tests
{
namespace fs = std::filesystem;
using foundation::core::uint8;

inline std::span<const uint8> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const uint8*>(text.data()), text.size()};
}

inline std::string ReadFileText(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

inline void WriteFile(const fs::path& path, std::string_view data)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/// Checked-in fixture below tests/fixtures.
inline fs::path GetFixture(std::string_view relative)
{
    return fs::path(LUDUS_SCENE_IMPORT_FIXTURES_DIR) / relative;
}

/// Fresh scratch directory removed on destruction.
struct ScratchDirectory final
{
    fs::path Path;
    ScratchDirectory()
    {
        std::string pattern = (fs::temp_directory_path() / "ludus-scene-import-XXXXXX").string();
        Path = mkdtemp(pattern.data());
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(Path, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
};
} // namespace ludus::scene_import_tests
