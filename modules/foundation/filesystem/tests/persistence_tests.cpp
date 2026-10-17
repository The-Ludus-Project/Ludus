#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>

#include "persistence_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
using filesystem_fixture::Fixture;
using filesystem_fixture::Publish;

TEST_CASE("Atomic publication preserves opened revisions and reports requested sync")
{
    Fixture fixture;
    fixture.Write("nested/data", "old");
    Directory root;
    WriteDirectory writer;
    File retained, current;
    REQUIRE(root.Open(fixture.Root).Succeeded());
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    REQUIRE(root.OpenRead("nested/data", retained).Succeeded());
    const auto saved = Publish(writer, "nested/data", "new-complete");
    REQUIRE(saved.Outcome.Succeeded());
    REQUIRE(saved.Published);
    REQUIRE(saved.FileSynced);
    REQUIRE(saved.DirectorySynced);
    REQUIRE(saved.Cleanup.Succeeded());
    REQUIRE(fixture.Temporaries() == 0);
    REQUIRE(fixture.Read("nested/data") == "new-complete");
    uint8 bytes[3]{};
    REQUIRE(retained.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'o');
    REQUIRE(root.OpenRead("nested/data", current).Succeeded());
    REQUIRE(current.Size() == 12);
    struct stat info{};
    REQUIRE(::stat(fixture.Path("nested/data").c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0600);
    WriteOptions options;
    options.Sync = SyncPolicy::PublishOnly;
    const auto empty = Publish(writer, "empty", "", options);
    REQUIRE(empty.Outcome.Succeeded());
    REQUIRE(empty.Published);
    REQUIRE_FALSE(empty.FileSynced);
    REQUIRE_FALSE(empty.DirectorySynced);
    options.Sync = SyncPolicy::File;
    const auto synced = Publish(writer, "empty", "", options);
    REQUIRE(synced.FileSynced);
    REQUIRE_FALSE(synced.DirectorySynced);
}
TEST_CASE("Publication admission is explicit bounded and preserves rejected destinations")
{
    Fixture fixture;
    Directory root;
    WriteDirectory writer;
    REQUIRE(root.Open(fixture.Root).Succeeded());
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    WriteOptions options;
    options.Condition = WriteCondition::Missing;
    REQUIRE(Publish(writer, "nested/data", "one", options).Published);
    REQUIRE(Publish(writer, "nested/data", "two", options).Outcome.Code == Status::Conflict);
    REQUIRE(fixture.Read("nested/data") == "one");
    options.Condition = WriteCondition::MatchingStamp;
    REQUIRE(root.Observe("nested/data", options.Expected).Succeeded());
    REQUIRE(Publish(writer, "nested/data", "two", options).Published);
    REQUIRE(Publish(writer, "nested/data", "stale", options).Outcome.Code == Status::Conflict);
    REQUIRE(Publish(writer, "absent", "new", options).Outcome.Code == Status::Conflict);
    options.Condition = WriteCondition::Any;
    options.MaxBytes = 2;
    REQUIRE(Publish(writer, "nested/data", "too-long", options).Outcome.Code == Status::LimitExceeded);
    REQUIRE(fixture.Read("nested/data") == "two");
    options.MaxBytes = 0;
    REQUIRE(Publish(writer, "empty", "", options).Published);
    REQUIRE(Publish(writer, "empty", "x", options).Outcome.Code == Status::LimitExceeded);
    REQUIRE(fixture.Temporaries() == 0);
    options.Sync = static_cast<SyncPolicy>(255);
    REQUIRE(Publish(writer, "empty", "", options).Outcome.Code == Status::InvalidArgument);
    options.Sync = SyncPolicy::File;
    options.Condition = static_cast<WriteCondition>(255);
    REQUIRE(Publish(writer, "empty", "", options).Outcome.Code == Status::InvalidArgument);
}
TEST_CASE("Write capabilities pin roots reject unsafe paths and retain ownership on failed opens")
{
    Fixture fixture;
    WriteDirectory writer;
    REQUIRE_FALSE(writer.IsOpen());
    REQUIRE(Publish(writer, "data", "x").Outcome.Code == Status::InvalidArgument);
    const auto alias = fixture.Path("trusted");
    std::filesystem::create_directory_symlink(fixture.Root, alias);
    REQUIRE(writer.Open(alias.string()).Succeeded());
    REQUIRE(writer.Open("/ludus-missing-f5").Code == Status::NotFound);
    REQUIRE(writer.Open("").Code == Status::InvalidArgument);
    REQUIRE(writer.Open(std::string_view("x\0y", 3)).Code == Status::InvalidArgument);
    REQUIRE(Publish(writer, "nested/音.dat", "sound").Published);
    for (const auto* bad : {"../escape", "/absolute", "nested//data", "nested/../data", "bad\\name"})
    {
        REQUIRE(Publish(writer, bad, "x").Outcome.Code == Status::InvalidArgument);
    }
    std::filesystem::create_symlink("nested/音.dat", fixture.Path("link"));
    std::filesystem::create_directory_symlink("/tmp", fixture.Path("escape"));
    REQUIRE(Publish(writer, "link", "x").Outcome.Code == Status::AccessDenied);
    REQUIRE_FALSE(Publish(writer, "escape/x", "x").Published);
    REQUIRE(Publish(writer, "nested", "x").Outcome.Code == Status::NotRegularFile);
    REQUIRE(::mkfifo(fixture.Path("fifo").c_str(), 0600) == 0);
    REQUIRE(Publish(writer, "fifo", "x").Outcome.Code == Status::NotRegularFile);
    REQUIRE(Publish(writer, "missing/data", "x").Outcome.Code == Status::NotFound);
    const auto moved = std::string(fixture.Root) + "-moved";
    std::filesystem::rename(fixture.Root, moved);
    REQUIRE(Publish(writer, "nested/kept", "kept").Published);
    std::filesystem::rename(moved, fixture.Root);
    WriteDirectory transferred(Move(writer));
    REQUIRE_FALSE(writer.IsOpen());
    writer = Move(transferred);
    REQUIRE_FALSE(transferred.IsOpen());
    REQUIRE(writer.Close().Succeeded());
    REQUIRE(writer.Close().Succeeded());
}
TEST_CASE("Cooperative parent locks refuse publication without blocking")
{
    Fixture fixture;
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    const int parent = ::open(fixture.Path("nested").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(parent >= 0);
    REQUIRE(::flock(parent, LOCK_EX | LOCK_NB) == 0);
    const auto refused = Publish(writer, "nested/data", "x");
    REQUIRE(refused.Outcome.Code == Status::Conflict);
    REQUIRE_FALSE(refused.Published);
    REQUIRE(fixture.Temporaries() == 0);
    REQUIRE(::close(parent) == 0);
    REQUIRE(Publish(writer, "nested/data", "x").Published);
}
TEST_CASE("Metadata observations preserve outputs reject unsafe leaves and identify replacements")
{
    Fixture fixture;
    fixture.Write("nested/data", "one");
    Directory root;
    REQUIRE(root.Open(fixture.Root).Succeeded());
    FileStamp stamp;
    REQUIRE(root.Observe("nested/data", stamp).Succeeded());
    REQUIRE(stamp.Size == 3);
    const auto original = stamp;
    REQUIRE(root.Observe("absent", stamp).Code == Status::NotFound);
    REQUIRE(stamp == original);
    REQUIRE(root.Observe("../data", stamp).Code == Status::InvalidArgument);
    REQUIRE(root.Observe("nested", stamp).Code == Status::NotRegularFile);
    std::filesystem::create_symlink("nested/data", fixture.Path("link"));
    REQUIRE(root.Observe("link", stamp).Code == Status::AccessDenied);
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    REQUIRE(Publish(writer, "nested/data", "two").Published);
    REQUIRE(root.Observe("nested/data", stamp).Succeeded());
    REQUIRE(stamp != original);
    REQUIRE(root.Close().Succeeded());
    REQUIRE(root.Observe("nested/data", stamp).Code == Status::InvalidArgument);
}
