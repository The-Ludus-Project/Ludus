#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include "pack_fixture.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
struct Fixture final
{
    char Path[64] = "/tmp/ludus-filesystem-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Path) != nullptr);
        std::filesystem::create_directory(std::filesystem::path(Path) / "nested");
        Write("nested/data", "abcdef");
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
TEST_CASE("Offset reads handle EOF empty files and ranges independently")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    REQUIRE(file.Size() == 6);
    uint8 bytes[8]{};
    auto read = file.ReadAt(3, bytes);
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(read.BytesRead == 3);
    REQUIRE(bytes[0] == 'd');
    read = file.ReadAt(0, {bytes, 2});
    REQUIRE(read.BytesRead == 2);
    REQUIRE(bytes[0] == 'a');
    REQUIRE(file.ReadAt(6, bytes).BytesRead == 0);
    REQUIRE(file.ReadAt(6, bytes).Outcome.Succeeded());
    REQUIRE(file.ReadAt(7, {}).Outcome.Code == Status::InvalidArgument);
    REQUIRE(file.ReadAt(~uint64{0}, bytes).Outcome.Code == Status::InvalidArgument);
    REQUIRE(file.ReadAt(0, {}).Outcome.Succeeded());
    fixture.Write("empty", "");
    REQUIRE(root.OpenRead("empty", file).Succeeded());
    REQUIRE(file.IsOpen());
    REQUIRE(file.Size() == 0);
    REQUIRE(file.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(file.ReadAt(0, bytes).BytesRead == 0);
}
TEST_CASE("Root and file capabilities retain revisions across rename and unlink")
{
    Fixture fixture;
    Directory root;
    File old, clone, current;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", old).Succeeded());
    fixture.Write("replacement", "NEW");
    std::filesystem::rename(std::filesystem::path(fixture.Path) / "replacement",
                            std::filesystem::path(fixture.Path) / "nested/data");
    REQUIRE(old.Clone(clone).Succeeded());
    REQUIRE(root.OpenRead("nested/data", current).Succeeded());
    REQUIRE(current.Size() == 3);
    uint8 bytes[6]{};
    REQUIRE(old.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'a');
    REQUIRE(clone.ReadAt(4, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'e');
    std::filesystem::remove(std::filesystem::path(fixture.Path) / "nested/data");
    REQUIRE(current.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'N');
    fixture.Write("kept", "K");
    const std::string renamed = std::string(fixture.Path) + "-moved";
    std::filesystem::rename(fixture.Path, renamed);
    REQUIRE(root.OpenRead("kept", current).Succeeded());
    REQUIRE(current.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'K');
    const auto result = root.OpenRead("empty", current);
    REQUIRE(result.Code == Status::NotFound);
    REQUIRE(result.NativeCode == ENOENT);
    std::filesystem::rename(renamed, fixture.Path);
}
TEST_CASE("Failed opens clones and moves preserve ownership")
{
    Fixture fixture;
    Directory root;
    File file, closed;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    REQUIRE(root.Open("/does-not-exist/ludus").Code == Status::NotFound);
    REQUIRE(root.Open(std::string_view("bad\0root", 8)).Code == Status::InvalidArgument);
    REQUIRE(root.OpenRead("missing/leaf", file).Code == Status::NotFound);
    REQUIRE(root.OpenRead("../escape", file).Code == Status::InvalidArgument);
    REQUIRE(file.IsOpen());
    REQUIRE(file.Size() == 6);
    REQUIRE(closed.Clone(file).Code == Status::InvalidArgument);
    REQUIRE(file.Size() == 6);
    REQUIRE(file.Clone(file).Succeeded());
    File moved(Move(file));
    REQUIRE_FALSE(file.IsOpen());
    REQUIRE(moved.Size() == 6);
    file = Move(moved);
    REQUIRE_FALSE(moved.IsOpen());
    REQUIRE(file.Close().Succeeded());
    REQUIRE(file.Close().Succeeded());
    REQUIRE(file.ReadAt(0, {}).Outcome.Code == Status::InvalidArgument);
    Directory movedRoot(Move(root));
    REQUIRE_FALSE(root.IsOpen());
    root = Move(movedRoot);
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    REQUIRE(root.Close().Succeeded());
    REQUIRE(file.IsOpen());
    REQUIRE(root.OpenRead("nested/data", file).Code == Status::InvalidArgument);
}
TEST_CASE("Child symlinks and non-regular files cannot be used as readable assets")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    std::filesystem::create_directory_symlink("/tmp", std::filesystem::path(fixture.Path) / "escape");
    std::filesystem::create_symlink("nested/data", std::filesystem::path(fixture.Path) / "link");
    REQUIRE_FALSE(root.OpenRead("escape/anything", file).Succeeded());
    const auto denied = root.OpenRead("link", file);
    REQUIRE(denied.Code == Status::AccessDenied);
    REQUIRE(denied.NativeCode == ELOOP);
    REQUIRE(root.OpenRead("nested", file).Code == Status::NotRegularFile);
    const auto fifo = std::filesystem::path(fixture.Path) / "fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    REQUIRE(root.OpenRead("fifo", file).Code == Status::NotRegularFile);
    REQUIRE(file.Size() == 6);
}
TEST_CASE("In-place edits fail revision checks including EOF and empty reads")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    fixture.Write("nested/data", "truncated");
    uint8 bytes[6]{};
    auto read = file.ReadAt(0, bytes);
    REQUIRE(read.Outcome.Code == Status::Changed);
    REQUIRE(read.BytesRead == 0);
    REQUIRE(file.ReadAt(file.Size(), {}).Outcome.Code == Status::Changed);
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    const auto path = std::filesystem::path(fixture.Path) / "nested/data";
    const timespec stamps[] = {{100, 0}, {100, 0}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), stamps, 0) == 0);
    REQUIRE(file.ReadAt(0, {}).Outcome.Code == Status::Changed);
}
TEST_CASE("Concurrent offset reads share one revision without a shared cursor")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    std::atomic<bool> valid{true};
    auto worker = [&](uint64 offset, uint8 expected) {
        for (usize i = 0; i < 500; ++i)
        {
            uint8 bytes[2]{};
            const auto read = file.ReadAt(offset, bytes);
            if (read.Outcome.Succeeded() == false || read.BytesRead != 2 || bytes[0] != expected)
            {
                valid.store(false);
            }
        }
    };
    std::thread first(worker, 0, 'a');
    std::thread second(worker, 3, 'd');
    first.join();
    second.join();
    REQUIRE(valid.load());
}

TEST_CASE("Native reads accept UTF-8 names and host-selected symlink roots")
{
    Fixture fixture;
    fixture.Write("音.wav", "sound");
    const auto link = std::filesystem::path(fixture.Path) / "trusted-root";
    std::filesystem::create_directory_symlink(fixture.Path, link);
    Directory root;
    File file;
    REQUIRE(root.Open(link.string()).Succeeded());
    REQUIRE(root.OpenRead("音.wav", file).Succeeded());
    uint8 bytes[5]{};
    REQUIRE(file.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 's');
    REQUIRE(bytes[4] == 'd');
}
TEST_CASE("Native offset reads preserve sparse file offsets above four GiB")
{
    Fixture fixture;
    const auto path = std::filesystem::path(fixture.Path) / "sparse";
    const int writer = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(writer >= 0);
    constexpr uint64 OFFSET = (uint64{1} << 32) + 17;
    const auto written = ::pwrite(writer, "end", 3, static_cast<off_t>(OFFSET));
    const auto closed = ::close(writer);
    REQUIRE(written == 3);
    REQUIRE(closed == 0);
    Directory root;
    File file, clone;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("sparse", file).Succeeded());
    REQUIRE(file.Size() == OFFSET + 3);
    REQUIRE(file.Clone(clone).Succeeded());
    uint8 bytes[4]{};
    const auto read = clone.ReadAt(OFFSET, bytes);
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(read.BytesRead == 3);
    REQUIRE(bytes[0] == 'e');
    REQUIRE(bytes[2] == 'd');
}
TEST_CASE("Revision checks detect nanosecond-only modification time changes")
{
    Fixture fixture;
    const auto path = std::filesystem::path(fixture.Path) / "nested/data";
    const timespec original[] = {{100, 0}, {100, 1}};
    const timespec changed[] = {{100, 0}, {100, 999999999}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), original, 0) == 0);
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("nested/data", file).Succeeded());
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), changed, 0) == 0);
    uint8 bytes[6]{};
    const auto read = file.ReadAt(0, bytes);
    REQUIRE(read.Outcome.Code == Status::Changed);
    REQUIRE(read.BytesRead == 0);
    REQUIRE(file.Size() == 6);
}

TEST_CASE("Native mounts retain opened revisions after publication and reject unsafe overlay fallback")
{
    Fixture fixture;
    ProviderHandle source;
    REQUIRE(CreateDirectoryProvider(fixture.Path, source).Succeeded());
    const auto* previous = source.Get();
    REQUIRE(CreateDirectoryProvider("/ludus-missing-root-for-f2", source).Code == Status::NotFound);
    REQUIRE(source.Get() == previous);
    const uint8 fallbackBytes[]{42};
    const MemoryEntry entry{"link", fallbackBytes};
    ProviderHandle fallback;
    REQUIRE(CreateMemoryProvider({&entry, 1}, fallback).Succeeded());
    Mount mounts[]{{Root::Assets, "files", 1, 1, source}, {Root::Assets, "files", 0, 2, fallback}};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
    VirtualPath key;
    REQUIRE(key.Set(Root::Assets, "files/nested/data").Succeeded());
    VirtualFile file;
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
    fixture.Write("replacement", "uvwxyz");
    REQUIRE(::rename((std::filesystem::path(fixture.Path) / "replacement").c_str(),
                     (std::filesystem::path(fixture.Path) / "nested/data").c_str()) == 0);
    REQUIRE(MountSnapshot::Create({}, 2, snapshot).Succeeded());
    mounts[0].Source = {};
    source = {};
    uint8 read[6]{};
    REQUIRE(file.ReadAt(0, read).BytesRead == 6);
    REQUIRE(read[0] == 'a');
    REQUIRE(read[5] == 'f');
    REQUIRE(file.Generation() == 1);

    REQUIRE(CreateDirectoryProvider(fixture.Path, mounts[0].Source).Succeeded());
    REQUIRE(MountSnapshot::Create(mounts, 3, snapshot).Succeeded());
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
    REQUIRE(file.ReadAt(0, read).BytesRead == 6);
    REQUIRE(read[0] == 'u');
    REQUIRE(::symlink("nested/data", (std::filesystem::path(fixture.Path) / "link").c_str()) == 0);
    REQUIRE(key.Set(Root::Assets, "files/link").Succeeded());
    MountAttempt attempts[2];
    const auto refused = snapshot.OpenRead(key, file, attempts);
    REQUIRE(refused.Outcome.Code == Status::AccessDenied);
    REQUIRE(refused.Attempts == 1);
    REQUIRE(attempts[0].Id == 1);
    REQUIRE(file.Generation() == 3);
    fixture.Write("nested/data", "changed-size");
    const auto changed = file.ReadAt(0, read);
    REQUIRE(changed.Outcome.Code == Status::Changed);
    REQUIRE(changed.BytesRead == 0);
}
TEST_CASE("Separate snapshot handles support concurrent memory opens and shared offset reads")
{
    const uint8 bytes[]{1, 2, 3, 4};
    const MemoryEntry entry{"data", bytes};
    ProviderHandle source;
    REQUIRE(CreateMemoryProvider({&entry, 1}, source).Succeeded());
    const Mount mount{Root::Assets, {}, 0, 1, source};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded());
    VirtualPath path;
    REQUIRE(path.Set(Root::Assets, "data").Succeeded());
    VirtualFile shared;
    REQUIRE(snapshot.OpenRead(path, shared).Outcome.Succeeded());
    std::atomic<bool> valid{true};
    auto worker = [&](const MountSnapshot& retained) {
        for (usize i = 0; i < 100; ++i)
        {
            VirtualFile opened, clone;
            uint8 read[2]{};
            if (retained.OpenRead(path, opened).Outcome.Succeeded() == false ||
                shared.Clone(clone).Succeeded() == false || clone.ReadAt(2, read).BytesRead != 2 || read[0] != 3 ||
                read[1] != 4 || opened.ReadAt(0, read).BytesRead != 2 || read[0] != 1 || read[1] != 2)
            {
                valid.store(false);
            }
        }
    };
    std::thread a(worker, snapshot), b(worker, snapshot), c(worker, snapshot);
    a.join();
    b.join();
    c.join();
    REQUIRE(valid.load());
}

TEST_CASE("Native packs retain replaced and unlinked revisions and detect in-place mutation")
{
    Fixture fixture;
    const auto bytes =
        std::string_view(reinterpret_cast<const char*>(pack_fixture::kPack), sizeof(pack_fixture::kPack));
    fixture.Write("archive", bytes);
    ProviderHandle storage, pack;
    REQUIRE(CreateDirectoryProvider(fixture.Path, storage).Succeeded());
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Succeeded());
    const Mount mount{Root::Assets, {}, 0, 1, pack};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded());
    VirtualPath key;
    REQUIRE(key.Set(Root::Assets, "compressed").Succeeded());
    VirtualFile file, clone;
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
    REQUIRE(file.Clone(clone).Succeeded());
    fixture.Write("replacement", bytes);
    REQUIRE(::rename((std::filesystem::path(fixture.Path) / "replacement").c_str(),
                     (std::filesystem::path(fixture.Path) / "archive").c_str()) == 0);
    REQUIRE(MountSnapshot::Create({}, 2, snapshot).Succeeded());
    pack = {};
    uint8 read[8]{};
    REQUIRE(clone.ReadAt(65532, read).Outcome.Succeeded());
    REQUIRE(read[0] == 'A');
    REQUIRE(::unlink((std::filesystem::path(fixture.Path) / "archive").c_str()) == 0);
    REQUIRE(file.ReadAt(70000, read).BytesRead == 8);
    REQUIRE(read[0] == 't');
    fixture.Write("archive", bytes);
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Succeeded());
    const Mount current{Root::Assets, {}, 0, 2, pack};
    REQUIRE(MountSnapshot::Create({&current, 1}, 3, snapshot).Succeeded());
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
    fixture.Write("archive", "changed");
    const auto changed = file.ReadAt(0, read);
    REQUIRE(changed.Outcome.Code == Status::Changed);
    REQUIRE(changed.BytesRead == 0);
    REQUIRE(file.ReadAt(file.Size(), {}).Outcome.Code == Status::Changed);
    REQUIRE(snapshot.OpenRead(key, clone).Outcome.Code == Status::Changed);
    REQUIRE(clone.Generation() == 1);
}
TEST_CASE("Concurrent pack clones serialize scratch while preserving independent offsets")
{
    const MemoryEntry entry{"archive", pack_fixture::kPack};
    ProviderHandle storage, pack;
    REQUIRE(CreateMemoryProvider({&entry, 1}, storage).Succeeded());
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Succeeded());
    const Mount mount{Root::Assets, {}, 0, 1, pack};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded());
    VirtualPath path;
    REQUIRE(path.Set(Root::Assets, "compressed").Succeeded());
    VirtualFile shared;
    REQUIRE(snapshot.OpenRead(path, shared).Outcome.Succeeded());
    std::atomic<bool> valid{true};
    auto worker = [&](uint64 offset, uint8 expected) {
        VirtualFile clone;
        if (shared.Clone(clone).Succeeded() == false)
        {
            valid.store(false);
            return;
        }
        for (usize i = 0; i < 50; ++i)
        {
            uint8 read[8]{};
            const auto result = clone.ReadAt(offset, read);
            if (result.Outcome.Succeeded() == false || result.BytesRead != 8 || read[0] != expected)
            {
                valid.store(false);
            }
        }
    };
    std::thread first(worker, 65532, 'A'), second(worker, 70000, 't');
    first.join();
    second.join();
    REQUIRE(valid.load());
}
