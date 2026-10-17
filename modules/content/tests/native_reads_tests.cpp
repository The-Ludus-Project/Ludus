#include <ludus/content/content.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <unistd.h>

using namespace ludus::content;
namespace
{
struct Fixture final
{
    char Path[64] = "/tmp/ludus-content-native-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Path) != nullptr);
        Write("data", "abc");
        Write("empty", "");
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Path, ignored);
    }
    void Write(const char* name, std::string_view data)
    {
        std::ofstream file(std::filesystem::path(Path) / name, std::ios::binary);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        REQUIRE(file.good());
    }
};
} // namespace
TEST_CASE("Native Content reads preserve capped failures and accept empty data")
{
    Fixture fixture;
    Bytes output;
    REQUIRE(output.Resize(1));
    output.Data()[0] = 'z';
    REQUIRE(ReadFile(fixture.Path, "data", 2, output) == Status::Limit);
    REQUIRE(output.Data().size() == 1);
    REQUIRE(output.Data()[0] == 'z');
    REQUIRE(ReadFile(fixture.Path, "missing/data", 3, output) == Status::NotFound);
    REQUIRE(output.Data()[0] == 'z');
    REQUIRE(ReadFile(fixture.Path, "../escape", 3, output) == Status::Invalid);
    REQUIRE(output.Data()[0] == 'z');
    REQUIRE(ReadFile(fixture.Path, "data", 3, output) == Status::Ok);
    REQUIRE(output.String() == "abc");
    REQUIRE(ReadFile(fixture.Path, "empty", 0, output) == Status::Ok);
    REQUIRE(output.Data().empty());
    FileReader empty;
    REQUIRE(empty.Open(fixture.Path, "empty") == Status::Ok);
    usize count = 99;
    uint8 bytes[1]{};
    REQUIRE(empty.Read(bytes, count) == Status::Ok);
    REQUIRE(count == 0);
    REQUIRE(empty.Seek(-1, true) == Status::Invalid);
}
TEST_CASE("Native Content readers retain replaced revisions and map in-place changes to conflicts")
{
    Fixture fixture;
    FileReader reader, clone, current;
    REQUIRE(reader.Open(fixture.Path, "data") == Status::Ok);
    REQUIRE(reader.Seek(1, false) == Status::Ok);
    REQUIRE(reader.Clone(clone) == Status::Ok);
    fixture.Write("replacement", "NEW!");
    std::filesystem::rename(std::filesystem::path(fixture.Path) / "replacement",
                            std::filesystem::path(fixture.Path) / "data");
    uint8 bytes[4]{};
    usize count = 0;
    REQUIRE(reader.Read(bytes, count) == Status::Ok);
    REQUIRE(count == 2);
    REQUIRE(bytes[0] == 'b');
    REQUIRE(clone.Read(bytes, count) == Status::Ok);
    REQUIRE(count == 3);
    REQUIRE(bytes[0] == 'a');
    REQUIRE(current.Open(fixture.Path, "data") == Status::Ok);
    REQUIRE(current.Size() == 4);
    fixture.Write("data", "changed in place");
    REQUIRE(current.Read(bytes, count) == Status::Conflict);
    REQUIRE(count == 0);
}
