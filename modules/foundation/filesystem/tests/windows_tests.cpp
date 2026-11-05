#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>
#include <ludus/foundation/filesystem/pack.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include "pack_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <thread>

#include <windows.h>
#include <winioctl.h>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;

namespace
{
constexpr std::string_view ASSET = "nested/音 data.bin";
constexpr std::string_view ARCHIVE = "nested/音 archive.lupk";
constexpr usize OLD_BYTE_COUNT = 3;
constexpr usize NEW_BYTE_COUNT = 4;
constexpr StaticArray<uint8, OLD_BYTE_COUNT> OLD_BYTES{{'o', 'l', 'd'}};
constexpr StaticArray<uint8, NEW_BYTE_COUNT> NEW_BYTES{{'n', 'e', 'w', '!'}};

struct Fixture final
{
    std::filesystem::path Root;
    std::string Utf8Root;
    WriteDirectory Writer;
    Directory Reader;

    Fixture()
    {
        const std::wstring name =
            L"ludus Windows 音 " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64());
        Root = std::filesystem::temp_directory_path() / name;
        REQUIRE(std::filesystem::create_directories(Root / L"nested") == true);
        const std::u8string encoded = Root.u8string();
        Utf8Root.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        REQUIRE(Writer.Open(Utf8Root).Succeeded() == true);
        REQUIRE(Reader.Open(Utf8Root).Succeeded() == true);
    }

    ~Fixture()
    {
        (void)Writer.Close();
        (void)Reader.Close();
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
};

std::span<const uint8> OldBytes()
{
    return {OLD_BYTES.GetData(), OLD_BYTES.GetSize()};
}

std::span<const uint8> NewBytes()
{
    return {NEW_BYTES.GetData(), NEW_BYTES.GetSize()};
}

PublicationResult PublishFile(const Fixture& fixture, std::string_view path, std::span<const uint8> bytes)
{
    return fixture.Writer.Publish(path, bytes, { .Sync = SyncPolicy::File, .Expected = {} });
}
} // namespace

TEST_CASE("Windows publication preserves pinned roots and opened file revisions", "[windows][filesystem]")
{
    Fixture fixture;
    REQUIRE(PublishFile(fixture, ASSET, OldBytes()).Outcome.Succeeded() == true);
    File old;
    REQUIRE(fixture.Reader.OpenRead(ASSET, old).Succeeded() == true);
    const std::filesystem::path moved = fixture.Root.native() + L" moved";
    std::filesystem::rename(fixture.Root, moved);
    fixture.Root = moved;

    WriteOptions options;
    REQUIRE(fixture.Reader.Observe(ASSET, options.Expected).Succeeded() == true);
    options.Condition = WriteCondition::MatchingStamp;
    const PublicationResult replaced = fixture.Writer.Publish(ASSET, NewBytes(), options);
    REQUIRE(replaced.Published == true);
    CHECK(replaced.FileSynced == true);
    CHECK(replaced.DirectorySynced == replaced.Outcome.Succeeded());
    CHECK(replaced.Cleanup.Succeeded() == true);
    CHECK(fixture.Writer.Publish(ASSET, OldBytes(), options).Outcome.Code == Status::Conflict);

    File clone;
    File current;
    REQUIRE(old.Clone(clone).Succeeded() == true);
    REQUIRE(fixture.Reader.OpenRead(ASSET, current).Succeeded() == true);
    uint8 byte = 0;
    CHECK(old.ReadAt(0, {&byte, 1}).BytesRead == 1);
    CHECK(byte == 'o');
    CHECK(clone.ReadAt(2, {&byte, 1}).BytesRead == 1);
    CHECK(byte == 'd');
    CHECK(current.ReadAt(3, {&byte, 1}).BytesRead == 1);
    CHECK(byte == '!');
    CHECK(current.ReadAt(4, {&byte, 1}).BytesRead == 0);
    CHECK(current.ReadAt(5, {}).Outcome.Code == Status::InvalidArgument);
    CHECK(fixture.Reader.OpenRead("nested/absent", current).Code == Status::NotFound);
    CHECK(current.Size() == NewBytes().size());
}

TEST_CASE("Windows publication reports sharing and limits without losing the accepted file", "[windows][filesystem]")
{
    Fixture fixture;
    REQUIRE(PublishFile(fixture, ASSET, OldBytes()).Published == true);
    const std::filesystem::path path = fixture.Root / L"nested" / L"音 data.bin";
    const HANDLE blocker = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(blocker != INVALID_HANDLE_VALUE);
    const PublicationResult blocked = PublishFile(fixture, ASSET, NewBytes());
    REQUIRE(CloseHandle(blocker) != FALSE);
    CHECK(blocked.Published == false);
    CHECK(blocked.Outcome.Code == Status::AccessDenied);
    CHECK(blocked.Cleanup.Succeeded() == true);
    File current;
    REQUIRE(fixture.Reader.OpenRead(ASSET, current).Succeeded() == true);
    CHECK(current.Size() == OldBytes().size());
    CHECK(fixture.Writer.Publish(ASSET, NewBytes(), { .MaxBytes = 1, .Expected = {} }).Outcome.Code ==
          Status::LimitExceeded);
    CHECK(fixture.Writer.Publish(ASSET, NewBytes(), { .Condition = WriteCondition::Missing, .Expected = {} })
              .Outcome.Code == Status::Conflict);
    CHECK(fixture.Reader.OpenRead("nested/音 data.bin.", current).Code == Status::InvalidArgument);
    CHECK(fixture.Reader.OpenRead("nested/.LUDUS-WRITE-LOCK", current).Code == Status::InvalidArgument);

    const std::filesystem::path lockPath = fixture.Root / L"nested" / L".ludus-write-lock";
    const HANDLE lock =
        CreateFileW(lockPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    const PublicationResult busy = PublishFile(fixture, ASSET, NewBytes());
    REQUIRE(CloseHandle(lock) != FALSE);
    CHECK(busy.Outcome.Code == Status::Conflict);
    CHECK(busy.Published == false);
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path.parent_path()))
    {
        CHECK((entry.path() == path || entry.path() == lockPath));
    }
    REQUIRE(PublishFile(fixture, ASSET, NewBytes()).Published == true);
}

TEST_CASE("Windows mounts native packs and reads their revisions through the async pool", "[windows][filesystem]")
{
    Fixture fixture;
    REQUIRE(PublishFile(fixture, ARCHIVE, pack_fixture::kPack).Published == true);
    constexpr uint8 BAD_ARCHIVE[] = {'n', 'o'};
    REQUIRE(PublishFile(fixture, "nested/broken.lupk", BAD_ARCHIVE).Published == true);

    ProviderHandle directory;
    ProviderHandle pack;
    REQUIRE(CreateDirectoryProvider(fixture.Utf8Root, directory).Succeeded() == true);
    REQUIRE(CreatePackProvider(directory, ARCHIVE, {}, pack).Succeeded() == true);
    CHECK(CreatePackProvider(directory, "nested/broken.lupk", {}, pack).Code == Status::CorruptData);
    const Mount mount{Root::Assets, {}, 0, 9, pack};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 71, snapshot).Succeeded() == true);
    VirtualPath key;
    VirtualFile file;
    REQUIRE(key.Set(Root::Assets, "raw").Succeeded() == true);
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded() == true);
    AsyncReader reader;
    uint8 byte = 0;
    REQUIRE(reader.Initialize({2, 1, 8, 5'000'000, 0}) == AsyncStatus::Ok);
    RequestHandle request;
    REQUIRE(reader.Submit(file, 17, {&byte, 1}, request) == AsyncStatus::Ok);
    ReadCompletion completion;
    const uint64 deadline = AsyncNow() + 5'000'000'000;
    while (reader.Poll(completion) == false && AsyncNow() < deadline)
    {
        std::this_thread::yield();
    }
    REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
    REQUIRE(completion.Handle.Scheduler == request.Scheduler);
    REQUIRE(completion.Handle.Sequence == request.Sequence);
    CHECK(completion.Read.Outcome.Succeeded() == true);
    CHECK(completion.Read.BytesRead == 1);
    CHECK(byte == 17);
}

TEST_CASE("Windows uses the full 64-bit offset for native reads", "[windows][filesystem]")
{
    Fixture fixture;
    const std::filesystem::path path = fixture.Root / L"sparse.bin";
    const HANDLE sparse = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(sparse != INVALID_HANDLE_VALUE);
    DWORD ignored = 0;
    const BOOL marked = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
    LARGE_INTEGER offset{};
    offset.QuadPart = (int64{1} << 32) + 17;
    const BOOL sought = SetFilePointerEx(sparse, offset, nullptr, FILE_BEGIN);
    DWORD written = 0;
    const uint8 expected = 42;
    const BOOL wrote = marked != FALSE && sought != FALSE ? WriteFile(sparse, &expected, 1, &written, nullptr) : FALSE;
    REQUIRE(CloseHandle(sparse) != FALSE);
    REQUIRE(marked != FALSE);
    REQUIRE(sought != FALSE);
    REQUIRE(wrote != FALSE);
    REQUIRE(written == 1);
    File file;
    REQUIRE(fixture.Reader.OpenRead("sparse.bin", file).Succeeded() == true);
    uint8 byte = 0;
    const ReadResult read = file.ReadAt(static_cast<uint64>(offset.QuadPart), {&byte, 1});
    CHECK(read.Outcome.Succeeded() == true);
    CHECK(read.BytesRead == 1);
    CHECK(byte == expected);
}

TEST_CASE("Windows watcher debounces native replacements and rescans on overflow", "[windows][filesystem]")
{
    Fixture fixture;
    Watcher watcher;
    REQUIRE(watcher.Init(fixture.Utf8Root, {2, 1, 10}).Succeeded() == true);
    WatchHandle asset;
    WatchHandle archive;
    REQUIRE(watcher.Add(ASSET, 1, asset).Succeeded() == true);
    REQUIRE(watcher.Add(ARCHIVE, 2, archive).Succeeded() == true);
    REQUIRE(PublishFile(fixture, ASSET, OldBytes()).Published == true);
    REQUIRE(watcher.Advance(1, 2).Succeeded() == true);
    WatchHint hint;
    CHECK(watcher.Poll(hint).Available == false);
    REQUIRE(PublishFile(fixture, ASSET, NewBytes()).Published == true);
    REQUIRE(watcher.Advance(5, 2).Succeeded() == true);
    REQUIRE(watcher.Advance(14, 2).Succeeded() == true);
    CHECK(watcher.Poll(hint).Available == false);
    REQUIRE(watcher.Advance(15, 2).Succeeded() == true);
    REQUIRE(watcher.Poll(hint).Available == true);
    CHECK(hint.Handle == asset);
    CHECK(hint.PathView() == ASSET);
    CHECK(hint.Stamp.Size == NewBytes().size());

    REQUIRE(PublishFile(fixture, ASSET, OldBytes()).Published == true);
    REQUIRE(PublishFile(fixture, ARCHIVE, pack_fixture::kPack).Published == true);
    REQUIRE(watcher.Advance(20, 2).Succeeded() == true);
    REQUIRE(watcher.Advance(30, 2).Code == Status::LimitExceeded);
    REQUIRE(watcher.Poll(hint).Available == true);
    CHECK(hint.Kind == WatchHintKind::RescanRequired);
    REQUIRE(watcher.BeginRescan().Succeeded() == true);
    REQUIRE(watcher.NextRescan(hint).Available == true);
    CHECK(hint.Handle == asset);
    REQUIRE(watcher.NextRescan(hint).Available == true);
    CHECK(hint.Handle == archive);
    CHECK(watcher.NextRescan(hint).Available == false);
    CHECK(watcher.NeedsRescan() == false);
}
