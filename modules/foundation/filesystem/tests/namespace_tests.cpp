#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>

#include <atomic>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
ProviderHandle Memory(uint8 value, std::string_view path = "data")
{
    const MemoryEntry entry{path, {&value, 1}};
    ProviderHandle result;
    REQUIRE(CreateMemoryProvider({&entry, 1}, result).Succeeded());
    return result;
}
VirtualPath Key(std::string_view path = "data", Root root = Root::Assets)
{
    VirtualPath key;
    REQUIRE(key.Set(root, path).Succeeded());
    return key;
}
uint8 ReadByte(const VirtualFile& file)
{
    uint8 value = 0;
    const auto read = file.ReadAt(0, {&value, 1});
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(read.BytesRead == 1);
    return value;
}
struct FaultState final
{
    usize Opens = 0;
    usize ProvidersDestroyed = 0;
    usize FilesDestroyed = 0;
};
class FaultFile final : public ProviderFile
{
public:
    explicit FaultFile(FaultState& state) noexcept : mState(state) {}
    ~FaultFile() noexcept override
    {
        ++mState.FilesDestroyed;
    }
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return 4;
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept override
    {
        if (offset > 4)
        {
            return {{Status::InvalidArgument}};
        }
        if (destination.empty() == false && offset < 4)
        {
            destination[0] = 42;
            return {{Status::IoError, 123}, 1};
        }
        return {};
    }

private:
    FaultState& mState;
};
class FaultProvider final : public Provider
{
public:
    FaultProvider(FaultState& state, Result outcome, bool broken = false) noexcept
        : mState(state), mOutcome(outcome), mBroken(broken)
    {
    }
    ~FaultProvider() noexcept override
    {
        ++mState.ProvidersDestroyed;
    }
    [[nodiscard]] Result OpenRead(std::string_view, ProviderFile*& output) const noexcept override
    {
        ++mState.Opens;
        if (mOutcome.Succeeded() && mBroken == false)
        {
            auto* next = new (std::nothrow) FaultFile(mState);
            if (next == nullptr)
            {
                return {Status::OutOfMemory};
            }
            output = next;
        }
        return mOutcome;
    }

private:
    FaultState& mState;
    Result mOutcome;
    bool mBroken;
};
} // namespace

TEST_CASE("Logical keys own their exact UTF-8 bytes and preserve valid output on failure")
{
    VirtualPath path;
    REQUIRE_FALSE(path.IsValid());
    REQUIRE(path.Path().empty());
    std::string source = "nested/音.wav";
    REQUIRE(path.Set(Root::Project, source).Succeeded());
    source[0] = 'x';
    REQUIRE(path.Path() == "nested/音.wav");
    REQUIRE(path.GetRoot() == Root::Project);
    for (const auto bad : {"", "/data", "a//b", ".", "a/../b", "a\\b", "assets:data", "a\x7f"})
    {
        REQUIRE(path.Set(Root::Assets, bad).Code == Status::InvalidArgument);
        REQUIRE(path.Path() == "nested/音.wav");
        REQUIRE(path.GetRoot() == Root::Project);
    }
    REQUIRE(path.Set(static_cast<Root>(255), "data").Code == Status::InvalidArgument);
    REQUIRE(path.Set(Root::User, path.Path().substr(7)).Succeeded());
    REQUIRE(path.Path() == "音.wav");
    REQUIRE(path.GetRoot() == Root::User);
    const std::string longest(MAX_PATH_BYTES, 'x');
    REQUIRE(path.Set(Root::Assets, longest).Succeeded());
    REQUIRE(path.Set(Root::Assets, longest + "x").Code == Status::InvalidArgument);
    REQUIRE(path.Path() == longest);
}

TEST_CASE("Memory providers copy input and use full byte-exact keys with independent revision lifetime")
{
    uint8 source[]{1, 2, 3, 4};
    std::string name = "nested/data";
    const MemoryEntry entries[]{{"z", {}}, {name, source}, {"Data", {source, 1}}, {"data", {source + 1, 1}}};
    ProviderHandle provider;
    REQUIRE(CreateMemoryProvider(entries, provider).Succeeded());
    source[0] = 99;
    name[0] = 'X';
    ProviderFile* revision = nullptr;
    REQUIRE(provider.Get()->OpenRead("nested/data", revision).Succeeded());
    provider = {};
    REQUIRE(revision->Size() == 4);
    uint8 bytes[6]{9, 9, 9, 9, 9, 9};
    auto read = revision->ReadAt(1, bytes);
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(read.BytesRead == 3);
    REQUIRE(bytes[0] == 2);
    REQUIRE(bytes[2] == 4);
    REQUIRE(bytes[3] == 9);
    read = revision->ReadAt(4, bytes);
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(read.BytesRead == 0);
    REQUIRE(revision->ReadAt(5, bytes).Outcome.Code == Status::InvalidArgument);
    REQUIRE(revision->ReadAt(~uint64{0}, bytes).Outcome.Code == Status::InvalidArgument);
    REQUIRE(revision->ReadAt(0, {}).Outcome.Succeeded());
    delete revision;

    auto exact = Memory(7);
    ProviderFile* file = nullptr;
    REQUIRE(exact.Get()->OpenRead("Data", file).Code == Status::NotFound);
    REQUIRE(file == nullptr);
    REQUIRE(exact.Get()->OpenRead("data", file).Succeeded());
    auto* before = file;
    REQUIRE(exact.Get()->OpenRead("missing", file).Code == Status::NotFound);
    REQUIRE(exact.Get()->OpenRead("../data", file).Code == Status::InvalidArgument);
    REQUIRE(file == before);
    delete file;

    ProviderHandle empty;
    REQUIRE(CreateMemoryProvider({}, empty).Succeeded());
    file = nullptr;
    REQUIRE(empty.Get()->OpenRead("data", file).Code == Status::NotFound);
    const MemoryEntry zero{"empty", {}};
    REQUIRE(CreateMemoryProvider({&zero, 1}, empty).Succeeded());
    REQUIRE(empty.Get()->OpenRead("empty", file).Succeeded());
    REQUIRE(file->Size() == 0);
    REQUIRE(file->ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(file->ReadAt(0, bytes).BytesRead == 0);
    delete file;
}

TEST_CASE("Memory construction rejects duplicate invalid and oversized indexes transactionally")
{
    auto provider = Memory(1);
    const auto* previous = provider.Get();
    const MemoryEntry duplicates[]{{"a", {}}, {"b", {}}, {"a", {}}};
    REQUIRE(CreateMemoryProvider(duplicates, provider).Code == Status::InvalidArgument);
    const MemoryEntry invalid{"../data", {}};
    REQUIRE(CreateMemoryProvider({&invalid, 1}, provider).Code == Status::InvalidArgument);
    const Array<MemoryEntry> oversized(MAX_MEMORY_FILES + 1);
    REQUIRE(CreateMemoryProvider(oversized, provider).Code == Status::InvalidArgument);
    REQUIRE(provider.Get() == previous);
}

TEST_CASE("Overlay priority and stable IDs determine lookup regardless of insertion order")
{
    const auto low = Memory(1);
    const auto tie = Memory(2);
    const auto high = Memory(3);
    Mount mounts[]{{Root::Assets, {}, -1, 40, low}, {Root::Assets, {}, 10, 20, tie}, {Root::Assets, {}, 10, 10, high}};
    for (int rotation = 0; rotation < 3; ++rotation)
    {
        std::swap(mounts[0], mounts[1]);
        std::swap(mounts[1], mounts[2]);
        MountSnapshot snapshot;
        REQUIRE(MountSnapshot::Create(mounts, 17, snapshot).Succeeded());
        REQUIRE(snapshot.Count() == 3);
        MountInfo info;
        REQUIRE(snapshot.Describe(0, info));
        REQUIRE(info.Id == 10);
        REQUIRE(snapshot.Describe(1, info));
        REQUIRE(info.Id == 20);
        REQUIRE(snapshot.Describe(2, info));
        REQUIRE(info.Id == 40);
        REQUIRE_FALSE(snapshot.Describe(3, info));
        REQUIRE(info.Id == 40);
        VirtualFile file;
        MountAttempt trace[2]{{999, {Status::Changed}}, {888, {Status::Changed}}};
        const auto result = snapshot.OpenRead(Key(), file, trace);
        REQUIRE(result.Outcome.Succeeded());
        REQUIRE(result.Attempts == 1);
        REQUIRE(trace[0].Id == 10);
        REQUIRE(trace[0].Outcome.Succeeded());
        REQUIRE(trace[1].Id == 888);
        REQUIRE(file.MountId() == 10);
        REQUIRE(file.Generation() == 17);
        REQUIRE(ReadByte(file) == 3);
    }
}

TEST_CASE("Roots and complete-component prefixes constrain mounts and copied prefixes stay immutable")
{
    const auto source = Memory(9);
    std::string prefix = "nested";
    Mount mounts[]{{Root::Project, {}, 100, 1, source}, {Root::Assets, prefix, 0, 2, source}};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
    prefix[0] = 'X';
    mounts[1].Prefix = "elsewhere";
    mounts[1].Source = {};
    MountInfo info;
    REQUIRE(snapshot.Describe(1, info));
    REQUIRE(info.Prefix == "nested");
    VirtualFile file;
    REQUIRE(snapshot.OpenRead(Key("nested/data"), file).Outcome.Succeeded());
    REQUIRE(file.MountId() == 2);
    for (const auto path : {"nested", "nestedness/data", "data", "Xested/data"})
    {
        const auto result = snapshot.OpenRead(Key(path), file);
        REQUIRE(result.Outcome.Code == Status::NotFound);
        REQUIRE(result.Attempts == 0);
        REQUIRE(file.MountId() == 2);
    }
    REQUIRE(snapshot.OpenRead(Key("data", Root::Project), file).Outcome.Succeeded());
    REQUIRE(file.MountId() == 1);
    REQUIRE(snapshot.OpenRead(Key("data", Root::Cache), file).Outcome.Code == Status::NotFound);
}

TEST_CASE("Only NotFound permits fallback and bounded traces preserve provider diagnostics")
{
    const auto fallback = Memory(8);
    for (const auto code : {Status::NotFound,
                            Status::InvalidArgument,
                            Status::AccessDenied,
                            Status::NotRegularFile,
                            Status::OutOfMemory,
                            Status::IoError,
                            Status::Changed,
                            Status::Unsupported,
                            Status::CorruptData,
                            Status::LimitExceeded})
    {
        FaultState state;
        const ProviderHandle fault(new FaultProvider(state, {code, 123}));
        const Mount mounts[]{{Root::Assets, {}, 2, 1, fault}, {Root::Assets, {}, 0, 2, fallback}};
        MountSnapshot snapshot;
        REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
        VirtualFile file;
        MountAttempt trace[1];
        const auto result = snapshot.OpenRead(Key(), file, trace);
        REQUIRE(state.Opens == 1);
        REQUIRE(trace[0].Id == 1);
        REQUIRE(trace[0].Outcome.Code == code);
        REQUIRE(trace[0].Outcome.NativeCode == 123);
        if (code == Status::NotFound)
        {
            REQUIRE(result.Outcome.Succeeded());
            REQUIRE(result.Attempts == 2);
            REQUIRE(ReadByte(file) == 8);
        }
        else
        {
            REQUIRE(result.Outcome.Code == code);
            REQUIRE(result.Outcome.NativeCode == 123);
            REQUIRE(result.Attempts == 1);
            REQUIRE_FALSE(file.IsOpen());
        }
    }
    FaultState state;
    const ProviderHandle broken(new FaultProvider(state, {}, true));
    const Mount mounts[]{{Root::Assets, {}, 2, 1, broken}, {Root::Assets, {}, 0, 2, fallback}};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
    VirtualFile file;
    REQUIRE(snapshot.OpenRead(Key(), file).Outcome.Code == Status::IoError);
    REQUIRE_FALSE(file.IsOpen());
}

TEST_CASE("All missing candidates produce NotFound while failed opens preserve the previous file")
{
    const auto source = Memory(7);
    const Mount mounts[]{{Root::Assets, {}, 1, 1, source}, {Root::Assets, {}, 0, 2, source}};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
    VirtualFile file;
    REQUIRE(snapshot.OpenRead(Key(), file).Outcome.Succeeded());
    MountAttempt trace[3]{{}, {}, {99, {Status::Changed}}};
    const auto missing = snapshot.OpenRead(Key("missing"), file, trace);
    REQUIRE(missing.Outcome.Code == Status::NotFound);
    REQUIRE(missing.Attempts == 2);
    REQUIRE(trace[0].Id == 1);
    REQUIRE(trace[1].Id == 2);
    REQUIRE(trace[2].Id == 99);
    REQUIRE(ReadByte(file) == 7);
    REQUIRE(snapshot.OpenRead(VirtualPath{}, file).Outcome.Code == Status::InvalidArgument);
    REQUIRE(ReadByte(file) == 7);
}

TEST_CASE("Unmount and reload retain provider file and snapshot until the final opened revision retires")
{
    FaultState state;
    VirtualFile file, clone;
    {
        ProviderHandle source(new FaultProvider(state, {}));
        const Mount mount{Root::Assets, {}, 0, 12, source};
        MountSnapshot snapshot;
        REQUIRE(MountSnapshot::Create({&mount, 1}, 3, snapshot).Succeeded());
        REQUIRE(snapshot.OpenRead(Key(), file).Outcome.Succeeded());
        REQUIRE(file.Clone(clone).Succeeded());
        REQUIRE(file.Clone(file).Succeeded());
        REQUIRE(MountSnapshot::Create({}, 4, snapshot).Succeeded());
        REQUIRE(snapshot.OpenRead(Key(), clone).Outcome.Code == Status::NotFound);
    }
    REQUIRE(state.ProvidersDestroyed == 0);
    REQUIRE(state.FilesDestroyed == 0);
    REQUIRE(file.Generation() == 3);
    uint8 bytes[4]{};
    const auto read = clone.ReadAt(0, bytes);
    REQUIRE(read.Outcome.Code == Status::IoError);
    REQUIRE(read.Outcome.NativeCode == 123);
    REQUIRE(read.BytesRead == 1);
    REQUIRE(bytes[0] == 42);
    REQUIRE(clone.ReadAt(4, bytes).Outcome.Succeeded());
    file.Close();
    REQUIRE(state.FilesDestroyed == 0);
    clone.Close();
    clone.Close();
    REQUIRE(state.FilesDestroyed == 1);
    REQUIRE(state.ProvidersDestroyed == 1);
    REQUIRE(clone.Size() == 0);
    REQUIRE(clone.Generation() == 0);
    REQUIRE(clone.MountId() == 0);
    REQUIRE(clone.Clone(file).Code == Status::InvalidArgument);
    REQUIRE(clone.ReadAt(0, bytes).Outcome.Code == Status::InvalidArgument);
}

TEST_CASE("Mount creation validates identities and preserves previous immutable snapshots")
{
    const auto source = Memory(1);
    Mount mount{Root::User, {}, 0, 1, source};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 5, snapshot).Succeeded());
    REQUIRE(MountSnapshot::Create({&mount, 1}, 0, snapshot).Code == Status::InvalidArgument);
    const Array<Mount> oversized(MAX_MOUNTS + 1);
    REQUIRE(MountSnapshot::Create(oversized, 6, snapshot).Code == Status::InvalidArgument);
    for (int fault = 0; fault < 4; ++fault)
    {
        auto bad = mount;
        switch (fault)
        {
            case 0:
                bad.Namespace = static_cast<Root>(255);
                break;
            case 1:
                bad.Prefix = "../bad";
                break;
            case 2:
                bad.Id = 0;
                break;
            case 3:
                bad.Source = {};
                break;
            default:
                break;
        }
        REQUIRE(MountSnapshot::Create({&bad, 1}, 6, snapshot).Code == Status::InvalidArgument);
    }
    const Mount duplicates[]{mount, mount};
    REQUIRE(MountSnapshot::Create(duplicates, 6, snapshot).Code == Status::InvalidArgument);
    REQUIRE(snapshot.Generation() == 5);
    auto shared = snapshot;
    const auto& self = snapshot;
    snapshot = self;
    REQUIRE(shared.Generation() == 5);
    MountSnapshot moved(Move(snapshot));
    REQUIRE_FALSE(snapshot.IsValid());
    REQUIRE(moved.Generation() == 5);
    snapshot = Move(moved);
    VirtualFile file;
    REQUIRE(MountSnapshot{}.OpenRead(Key(), file).Outcome.Code == Status::InvalidArgument);
    REQUIRE(snapshot.OpenRead(Key("data", Root::User), file).Outcome.Succeeded());
    VirtualFile movedFile(Move(file));
    REQUIRE_FALSE(file.IsOpen());
    REQUIRE(ReadByte(movedFile) == 1);
    file = Move(movedFile);
    REQUIRE(ReadByte(file) == 1);
    REQUIRE(MountSnapshot::Create({}, 6, snapshot).Succeeded());
    REQUIRE(snapshot.IsValid());
    REQUIRE(snapshot.Count() == 0);
    REQUIRE(snapshot.OpenRead(Key(), file).Outcome.Code == Status::NotFound);
}

TEST_CASE("Logical identity distinguishes roots case and Unicode normalization forms")
{
    REQUIRE(Key("data") == Key("data"));
    REQUIRE_FALSE(Key("data") == Key("Data"));
    REQUIRE_FALSE(Key("data") == Key("data", Root::Project));
    REQUIRE_FALSE(Key("é") == Key("e\xcc\x81"));
    REQUIRE(VirtualPath{} == VirtualPath{});
    const uint8 composed[]{1}, decomposed[]{2}, upper[]{3};
    const MemoryEntry entries[]{{"é", composed}, {"e\xcc\x81", decomposed}, {"É", upper}};
    ProviderHandle source;
    REQUIRE(CreateMemoryProvider(entries, source).Succeeded());
    const Mount mount{Root::Assets, {}, 0, 1, source};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded());
    VirtualFile file;
    REQUIRE(snapshot.OpenRead(Key("é"), file).Outcome.Succeeded());
    REQUIRE(ReadByte(file) == 1);
    REQUIRE(snapshot.OpenRead(Key("e\xcc\x81"), file).Outcome.Succeeded());
    REQUIRE(ReadByte(file) == 2);
    REQUIRE(snapshot.OpenRead(Key("É"), file).Outcome.Succeeded());
    REQUIRE(ReadByte(file) == 3);
}
