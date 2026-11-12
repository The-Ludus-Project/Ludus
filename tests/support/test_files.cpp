#include "test_files.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

// The one translation unit that parses <filesystem> for the fixtures linking
// ludus_test_files. It is compiled without exceptions like engine code: every
// call uses the std::error_code overloads, and allocation failure terminates.

namespace ludus::test_support
{
namespace
{
namespace fs = std::filesystem;
using foundation::uint32;
using foundation::usize;

/// Attempts before CreateScratchDirectory gives up; random collisions are astronomically rare.
constexpr uint32 MAX_SCRATCH_ATTEMPTS = 64;
/// Bits encoded by one hexadecimal digit of the scratch suffix.
constexpr uint32 HEX_DIGIT_BITS = 4;
/// Mask selecting one hexadecimal digit.
constexpr uint32 HEX_DIGIT_MASK = 0xF;
/// Hexadecimal digits in a 32-bit scratch suffix.
constexpr uint32 SUFFIX_DIGITS = 32 / HEX_DIGIT_BITS;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

[[nodiscard]] bool HasFailed(const std::error_code& error) noexcept
{
    return error.value() != 0;
}

[[nodiscard]] fs::path MakePath(std::string_view text)
{
    // Interpret the text as UTF-8 on every host, including Windows, where a char path is ANSI.
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

[[nodiscard]] std::string_view GetView(const std::u8string& text) noexcept
{
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] bool AssignText(std::string_view text, foundation::String& outText) noexcept
{
    return outText.TryAssign(text) == foundation::StringStatus::Ok;
}

[[nodiscard]] bool ReportEntry(const fs::directory_entry& entry, DirectoryEntryVisitor visitor, void* context)
{
    std::error_code error;
    const std::u8string path = entry.path().u8string();
    const std::u8string name = entry.path().filename().u8string();
    DirectoryEntry reported{};
    reported.Path = GetView(path);
    reported.Name = GetView(name);
    reported.IsRegularFile = entry.is_regular_file(error);
    if (HasFailed(error) == true)
    {
        return false;
    }
    reported.IsDirectory = entry.is_directory(error);
    if (HasFailed(error) == true)
    {
        return false;
    }
    visitor(reported, context);
    return true;
}

template <typename Iterator>
[[nodiscard]] bool VisitEntries(const fs::path& directory, DirectoryEntryVisitor visitor, void* context)
{
    std::error_code error;
    Iterator iterator(directory, error);
    if (HasFailed(error) == true)
    {
        return false;
    }
    const Iterator end;
    while (iterator != end)
    {
        if (ReportEntry(*iterator, visitor, context) == false)
        {
            return false;
        }
        iterator.increment(error);
        if (HasFailed(error) == true)
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool CreateScratchDirectory(std::string_view prefix, foundation::String& outPath) noexcept
{
    std::error_code error;
    const fs::path parent = fs::temp_directory_path(error);
    if (HasFailed(error) == true)
    {
        return false;
    }
    std::random_device entropy;
    for (uint32 attempt = 0; attempt < MAX_SCRATCH_ATTEMPTS; ++attempt)
    {
        const uint32 suffix = entropy();
        std::string name(prefix);
        name.push_back('-');
        for (uint32 digit = SUFFIX_DIGITS; digit > 0; --digit)
        {
            name.push_back(HEX_DIGITS[(suffix >> ((digit - 1) * HEX_DIGIT_BITS)) & HEX_DIGIT_MASK]);
        }
        const fs::path candidate = parent / MakePath(name);
        if (fs::create_directory(candidate, error) == true)
        {
            const std::u8string path = candidate.u8string();
            return AssignText(GetView(path), outPath);
        }
        if (HasFailed(error) == true)
        {
            return false;
        }
    }
    return false;
}

bool CreateDirectories(std::string_view path) noexcept
{
    const fs::path directory = MakePath(path);
    std::error_code error;
    fs::create_directories(directory, error);
    return HasFailed(error) == false && fs::is_directory(directory, error) == true;
}

bool RemoveTree(std::string_view path) noexcept
{
    std::error_code error;
    fs::remove_all(MakePath(path), error);
    return HasFailed(error) == false;
}

bool CopyTree(std::string_view source, std::string_view destination) noexcept
{
    std::error_code error;
    fs::copy(MakePath(source), MakePath(destination), fs::copy_options::recursive, error);
    return HasFailed(error) == false;
}

bool CopyRegularFile(std::string_view source, std::string_view destination) noexcept
{
    std::error_code error;
    return fs::copy_file(MakePath(source), MakePath(destination), error) == true && HasFailed(error) == false;
}

bool RemoveFile(std::string_view path) noexcept
{
    std::error_code error;
    return fs::remove(MakePath(path), error) == true && HasFailed(error) == false;
}

bool ReadFile(std::string_view path, foundation::String& outBytes) noexcept
{
    const fs::path file = MakePath(path);
    std::error_code error;
    const uint64 size = static_cast<uint64>(fs::file_size(file, error));
    if (HasFailed(error) == true)
    {
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (stream.is_open() == false)
    {
        return false;
    }
    std::string bytes(static_cast<usize>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
    {
        return false;
    }
    return AssignText(bytes, outBytes);
}

bool WriteFile(std::string_view path, std::span<const uint8> bytes) noexcept
{
    const fs::path file = MakePath(path);
    std::error_code error;
    if (file.has_parent_path() == true)
    {
        fs::create_directories(file.parent_path(), error);
        if (HasFailed(error) == true)
        {
            return false;
        }
    }
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    return stream.fail() == false;
}

bool IsExistingPath(std::string_view path) noexcept
{
    std::error_code error;
    return fs::exists(MakePath(path), error) == true;
}

bool IsDirectory(std::string_view path) noexcept
{
    std::error_code error;
    return fs::is_directory(MakePath(path), error) == true;
}

bool GetFileSize(std::string_view path, uint64& outBytes) noexcept
{
    std::error_code error;
    const uint64 size = static_cast<uint64>(fs::file_size(MakePath(path), error));
    if (HasFailed(error) == true)
    {
        return false;
    }
    outBytes = size;
    return true;
}

bool GetCanonicalPath(std::string_view path, foundation::String& outPath) noexcept
{
    std::error_code error;
    const fs::path canonical = fs::canonical(MakePath(path), error);
    if (HasFailed(error) == true)
    {
        return false;
    }
    const std::u8string text = canonical.generic_u8string();
    return AssignText(GetView(text), outPath);
}

bool VisitDirectory(std::string_view directory,
                    DirectoryWalk walk,
                    DirectoryEntryVisitor visitor,
                    void* context) noexcept
{
    LUDUS_ASSERT(visitor != nullptr);
    LUDUS_ASSERT(context != nullptr);
    const fs::path root = MakePath(directory);
    if (walk == DirectoryWalk::Recursive)
    {
        return VisitEntries<fs::recursive_directory_iterator>(root, visitor, context);
    }
    return VisitEntries<fs::directory_iterator>(root, visitor, context);
}
} // namespace ludus::test_support
