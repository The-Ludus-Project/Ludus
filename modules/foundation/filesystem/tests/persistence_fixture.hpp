#pragma once

#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/strings/string.hpp>

#include "test_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>
#include <string_view>

#include <unistd.h>

// Host file operations go through ludus_test_files, so including this fixture
// does not parse <filesystem> (docs/development/build-profiling.md).

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
        REQUIRE(ludus::test_support::CreateDirectories(Path("nested")) == true);
    }
    ~Fixture()
    {
        static_cast<void>(ludus::test_support::RemoveTree(Root));
    }
    [[nodiscard]] std::string Path(std::string_view name) const
    {
        std::string path(Root);
        path.push_back('/');
        path.append(name);
        return path;
    }
    void Write(const char* name, std::string_view data) const
    {
        REQUIRE(ludus::test_support::WriteFile(Path(name),
                                               {reinterpret_cast<const uint8*>(data.data()), data.size()}) == true);
    }
    [[nodiscard]] std::string Read(std::string_view name) const
    {
        String bytes;
        REQUIRE(ludus::test_support::ReadFile(Path(name), bytes) == true);
        return std::string(bytes.GetView());
    }
    [[nodiscard]] usize Temporaries() const
    {
        usize count = 0;
        const ludus::test_support::DirectoryEntryVisitor countTemporary =
            [](const ludus::test_support::DirectoryEntry& entry, void* context) noexcept {
                if (entry.Name.starts_with(".ludus-write-") == true)
                {
                    ++*static_cast<usize*>(context);
                }
            };
        REQUIRE(ludus::test_support::VisitDirectory(Path("nested"),
                                                    ludus::test_support::DirectoryWalk::Immediate,
                                                    countTemporary,
                                                    &count) == true);
        return count;
    }
};
inline PublicationResult
Publish(const WriteDirectory& writer, std::string_view name, std::string_view data, const WriteOptions& options = {})
{
    return writer.Publish(name, {reinterpret_cast<const uint8*>(data.data()), data.size()}, options);
}
} // namespace filesystem_fixture
