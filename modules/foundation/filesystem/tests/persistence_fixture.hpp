#pragma once

#include <ludus/foundation/filesystem/persistence.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <unistd.h>

namespace filesystem_fixture
{
using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
struct Fixture final
{
    char Root[64] = "/tmp/ludus-filesystem-f5-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Root) != nullptr);
        std::filesystem::create_directory(std::filesystem::path(Root) / "nested");
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
    [[nodiscard]] std::filesystem::path Path(std::string_view name) const
    {
        return std::filesystem::path(Root) / name;
    }
    void Write(const char* name, std::string_view data) const
    {
        std::ofstream stream(Path(name), std::ios::binary);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.close();
        REQUIRE(stream.good());
    }
    [[nodiscard]] std::string Read(std::string_view name) const
    {
        std::ifstream stream(Path(name), std::ios::binary);
        REQUIRE(stream.good());
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] usize Temporaries() const
    {
        usize count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(Path("nested")))
        {
            if (entry.path().filename().string().starts_with(".ludus-write-"))
            {
                ++count;
            }
        }
        return count;
    }
};
inline PublicationResult
Publish(const WriteDirectory& writer, std::string_view name, std::string_view data, const WriteOptions& options = {})
{
    return writer.Publish(name, {reinterpret_cast<const uint8*>(data.data()), data.size()}, options);
}
} // namespace filesystem_fixture
