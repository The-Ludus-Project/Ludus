#include <ludus/content/content.h>
#include <ludus/content/limits.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ludus::content;
namespace
{
struct Fixture final
{
    char Root[64] = "/tmp/ludus-content-save-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Root) != nullptr);
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
        std::ofstream file(Path(name), std::ios::binary);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        REQUIRE(file.good());
    }
};
constexpr uint8 INITIAL[] = {'a', 'b', 'c'};
constexpr uint8 NEXT[] = {'x', 'y'};
} // namespace

TEST_CASE("Native saves require absence or the current digest and publish empty files", "[content][save]")
{
    Fixture fixture;
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, nullptr) == Status::Ok);
    const auto digest = Hash(INITIAL);
    REQUIRE(SaveFile(fixture.Root, "data", NEXT, nullptr) == Status::Conflict);
    REQUIRE(SaveFile(fixture.Root, "absent", NEXT, &digest) == Status::Conflict);
    REQUIRE(SaveFile(fixture.Root, "data", NEXT, &digest) == Status::Ok);
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, &digest) == Status::Conflict);
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "data", 3, output) == Status::Ok);
    REQUIRE(output.String() == "xy");
    const auto nextDigest = Hash(NEXT);
    REQUIRE(SaveFile(fixture.Root, "data", {}, &nextDigest) == Status::Ok);
    REQUIRE(ReadFile(fixture.Root, "data", 0, output) == Status::Ok);
    REQUIRE(output.Data().empty());
    REQUIRE(SaveFile(fixture.Root, "empty", {}, nullptr) == Status::Ok);
    REQUIRE_FALSE(std::filesystem::exists(fixture.Path("data.ludus-save")));
    struct stat info{};
    REQUIRE(::stat(fixture.Path("data").c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0600);
}

TEST_CASE("Native saves validate roots and traverse existing UTF-8 parents", "[content][save]")
{
    Fixture fixture;
    std::filesystem::create_directory(fixture.Path("audio"));
    REQUIRE(SaveFile(fixture.Root, "audio/音.json", INITIAL, nullptr) == Status::Ok);
    std::filesystem::create_directory_symlink(fixture.Root, fixture.Path("root-alias"));
    REQUIRE(SaveFile(fixture.Path("root-alias").string(), "alias-data", INITIAL, nullptr) == Status::Ok);
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "alias-data", 3, output) == Status::Ok);
    REQUIRE(output.String() == "abc");
    REQUIRE(SaveFile(fixture.Root, "missing/data", INITIAL, nullptr) == Status::IoError);
    for (const auto path : {"../escape", "/absolute", "bad//name", "bad\\name", "bad/./name", "bad/../name"})
    {
        REQUIRE(SaveFile(fixture.Root, path, INITIAL, nullptr) == Status::Invalid);
    }
    REQUIRE(SaveFile({}, "data", INITIAL, nullptr) == Status::Invalid);
    REQUIRE(SaveFile(std::string_view("bad\0root", 8), "data", INITIAL, nullptr) == Status::Invalid);
    REQUIRE(SaveFile(std::string(4097, 'a'), "data", INITIAL, nullptr) == Status::Invalid);
    REQUIRE(SaveFile(fixture.Root, std::string_view("bad\0name", 8), INITIAL, nullptr) == Status::Invalid);
}

TEST_CASE("Native saves reject symlink children and non-regular destinations", "[content][save]")
{
    Fixture fixture, outside;
    outside.Write("data", "outside");
    std::filesystem::create_directory_symlink(outside.Root, fixture.Path("escape"));
    REQUIRE(SaveFile(fixture.Root, "escape/new", INITIAL, nullptr) != Status::Ok);
    REQUIRE_FALSE(std::filesystem::exists(outside.Path("new")));
    std::filesystem::create_symlink(outside.Path("data"), fixture.Path("link"));
    const uint8 external[] = {'o', 'u', 't', 's', 'i', 'd', 'e'};
    const auto digest = Hash(external);
    REQUIRE(SaveFile(fixture.Root, "link", INITIAL, &digest) == Status::IoError);
    std::filesystem::create_symlink(outside.Path("missing"), fixture.Path("dangling"));
    REQUIRE(SaveFile(fixture.Root, "dangling", INITIAL, nullptr) == Status::IoError);
    REQUIRE(std::filesystem::is_symlink(fixture.Path("dangling")));
    std::filesystem::create_directory(fixture.Path("directory"));
    REQUIRE(SaveFile(fixture.Root, "directory", INITIAL, nullptr) == Status::IoError);
    REQUIRE(::mkfifo(fixture.Path("fifo").c_str(), 0600) == 0);
    REQUIRE(SaveFile(fixture.Root, "fifo", INITIAL, nullptr) == Status::IoError);
    Bytes output;
    REQUIRE(ReadFile(outside.Root, "data", 7, output) == Status::Ok);
    REQUIRE(output.String() == "outside");
}

TEST_CASE("Native saves preserve foreign temporaries and fail promptly on a held directory lock", "[content][save]")
{
    Fixture fixture, outside;
    fixture.Write("data.ludus-save", "foreign");
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, nullptr) == Status::IoError);
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "data.ludus-save", 7, output) == Status::Ok);
    REQUIRE(output.String() == "foreign");
    REQUIRE_FALSE(std::filesystem::exists(fixture.Path("data")));
    std::filesystem::remove(fixture.Path("data.ludus-save"));
    outside.Write("data", "foreign");
    std::filesystem::create_symlink(outside.Path("data"), fixture.Path("data.ludus-save"));
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, nullptr) == Status::IoError);
    REQUIRE(std::filesystem::is_symlink(fixture.Path("data.ludus-save")));
    std::filesystem::remove(fixture.Path("data.ludus-save"));
    const int directory = ::open(fixture.Root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(directory >= 0);
    const int lock = ::flock(directory, LOCK_EX | LOCK_NB);
    const auto status = SaveFile(fixture.Root, "data", INITIAL, nullptr);
    const int closed = ::close(directory);
    REQUIRE(lock == 0);
    REQUIRE(closed == 0);
    REQUIRE(status == Status::Conflict);
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, nullptr) == Status::Ok);
}

TEST_CASE("Native save byte admission preserves the destination above 32 MiB", "[content][save]")
{
    Fixture fixture;
    REQUIRE(SaveFile(fixture.Root, "data", INITIAL, nullptr) == Status::Ok);
    const Digest digest = Hash(INITIAL);
    Bytes large;
    REQUIRE(large.Resize(MAX_SAVE_BYTES + 1) == true);
    REQUIRE(SaveFile(fixture.Root, "data", large.Data(), &digest) == Status::Limit);
    REQUIRE(SaveFile(fixture.Root, "absent", large.Data(), nullptr) == Status::Limit);
    REQUIRE_FALSE(std::filesystem::exists(fixture.Path("absent")));
    REQUIRE_FALSE(std::filesystem::exists(fixture.Path("data.ludus-save")));
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "data", 3, output) == Status::Ok);
    REQUIRE(output.String() == "abc");
}

TEST_CASE("Native saves admit the exact byte ceiling for both writes and replacements", "[content][save]")
{
    Fixture fixture;
    Bytes input;
    REQUIRE(input.Resize(MAX_SAVE_BYTES) == true);
    for (uint8& byte : input.Data())
    {
        byte = 0;
    }
    REQUIRE(SaveFile(fixture.Root, "data", input.Data(), nullptr) == Status::Ok);
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "data", MAX_SAVE_BYTES, output) == Status::Ok);
    REQUIRE(output.Data().size() == MAX_SAVE_BYTES);
    const Digest digest = Hash(output.Data());
    REQUIRE(SaveFile(fixture.Root, "data", NEXT, &digest) == Status::Ok);
    REQUIRE(ReadFile(fixture.Root, "data", MAX_SAVE_BYTES, output) == Status::Ok);
    CHECK(output.String() == "xy");
}
