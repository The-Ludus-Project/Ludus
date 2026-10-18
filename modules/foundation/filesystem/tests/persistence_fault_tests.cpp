#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>

#include "internal/persistence_test_hooks.hpp"
#include "persistence_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <filesystem>
#include <span>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(LUDUS_PLATFORM_LINUX)
#    include <sys/syscall.h>
#else
#    include <sys/stdio.h>
#endif

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
using filesystem_fixture::Fixture;
using filesystem_fixture::Publish;
namespace
{
enum class Fault : uint8
{
    None,
    Allocation,
    RootOpen,
    ParentOpen,
    AncestorOpen,
    TemporaryOpen,
    FirstMetadata,
    SecondMetadata,
    Lock,
    InterruptedLock,
    InterruptedMetadata,
    InterruptedOpen,
    InterruptedWrite,
    ShortWrite,
    PartialWrite,
    ZeroWrite,
    FileSync,
    DirectorySync,
    InterruptedSync,
    TemporaryClose,
    ReusedClose,
    ParentClose,
    Replace,
    Cleanup,
    Collisions,
    ExternalEdit,
    ExternalCreate,
    Reentrant,
};
Fault gFault = Fault::None;
int gTemporary = -1;
int gParent = -1;
int gSyncs = 0;
int gWrites = 0;
int gMetadata = 0;
int gTemporaryCloses = 0;
int gReused = -1;
int gCollisionCount = 0;
bool gInjected = false;
const WriteDirectory* gWriter = nullptr;
Status gReentrant = Status::Ok;
void Reset(Fault fault) noexcept
{
    if (gReused >= 0)
    {
        (void)::close(gReused);
        gReused = -1;
    }
    gFault = fault;
    gTemporary = gParent = -1;
    gSyncs = gWrites = gMetadata = gTemporaryCloses = gCollisionCount = 0;
    gInjected = false;
    gWriter = nullptr;
    gReentrant = Status::Ok;
}
int Error(int error = EIO) noexcept
{
    errno = error;
    return -1;
}
bool Once(Fault fault) noexcept
{
    if (gFault == fault && gInjected == false)
    {
        gInjected = true;
        return true;
    }
    return false;
}
} // namespace
namespace ludus::foundation::filesystem::persistence_test
{
bool AllocationAllowed() noexcept
{
    return gFault != Fault::Allocation;
}
int OpenAt(int parent, const char* name, int flags, mode_t mode) noexcept
{
    if (Once(Fault::InterruptedOpen))
    {
        return Error(EINTR);
    }
    const std::string_view path(name);
    if ((gFault == Fault::RootOpen && parent == AT_FDCWD) || (gFault == Fault::ParentOpen && path == ".") ||
        (gFault == Fault::AncestorOpen && path == "nested") ||
        (gFault == Fault::TemporaryOpen && (flags & O_CREAT) != 0))
    {
        return Error();
    }
    if (gFault == Fault::Collisions && (flags & O_CREAT) != 0)
    {
        ++gCollisionCount;
        // Real foreign collisions must survive every unsuccessful admission.
        const int foreign = ::openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (foreign >= 0)
        {
            (void)::write(foreign, "foreign", 7);
            (void)::close(foreign);
        }
        return Error(EEXIST);
    }
    const int value = ::openat(parent, name, flags, mode);
    if (value >= 0 && (flags & O_CREAT) != 0)
    {
        gTemporary = value;
        gParent = parent;
    }
    return value;
}
int MetadataAt(int parent, const char* name, struct stat* info, int flags) noexcept
{
    if (Once(Fault::InterruptedMetadata))
    {
        return Error(EINTR);
    }
    ++gMetadata;
    if ((gFault == Fault::FirstMetadata && gMetadata == 1) || (gFault == Fault::SecondMetadata && gMetadata == 2))
    {
        return Error();
    }
    return ::fstatat(parent, name, info, flags);
}
int Lock(int descriptor, int operation) noexcept
{
    if (gFault == Fault::Lock)
    {
        return Error(EWOULDBLOCK);
    }
    if (Once(Fault::InterruptedLock))
    {
        return Error(EINTR);
    }
    return ::flock(descriptor, operation);
}
isize Write(int descriptor, std::span<const uint8> bytes) noexcept
{
    ++gWrites;
    if (Once(Fault::InterruptedWrite))
    {
        return Error(EINTR);
    }
    if (gFault == Fault::ZeroWrite || gFault == Fault::Cleanup)
    {
        return 0;
    }
    if (gFault == Fault::PartialWrite && gWrites > 1)
    {
        return Error();
    }
    if (gFault == Fault::ShortWrite || gFault == Fault::PartialWrite)
    {
        return ::write(descriptor, bytes.data(), 1);
    }
    if (Once(Fault::ExternalEdit) || Once(Fault::ExternalCreate))
    {
        const int external = ::openat(gParent, "data", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (external < 0)
        {
            return Error();
        }
        (void)::write(external, "external-change", 15);
        (void)::close(external);
    }
    if (Once(Fault::Reentrant) && gWriter != nullptr)
    {
        // Same root capability, nested independent transaction: dup(root) would
        // share flock ownership and incorrectly admit this second publication.
        gReentrant = Publish(*gWriter, "nested/second", "second").Outcome.Code;
    }
    return ::write(descriptor, bytes.data(), bytes.size());
}
int Sync(int descriptor) noexcept
{
    if (Once(Fault::InterruptedSync))
    {
        return Error(EINTR);
    }
    ++gSyncs;
    if ((gFault == Fault::FileSync && gSyncs == 1) || (gFault == Fault::DirectorySync && gSyncs == 2))
    {
        return Error();
    }
    return ::fsync(descriptor);
}
int Replace(int parent, const char* source, const char* target) noexcept
{
    if (gFault == Fault::Replace)
    {
        return Error();
    }
#if defined(LUDUS_PLATFORM_LINUX)
    return static_cast<int>(::syscall(SYS_renameat, parent, source, parent, target));
#else
    return ::renameat(parent, source, parent, target);
#endif
}
int Unlink(int parent, const char* name) noexcept
{
    if (gFault == Fault::Cleanup)
    {
        return Error();
    }
    return ::unlinkat(parent, name, 0);
}
int Close(int descriptor) noexcept
{
    const int result = ::close(descriptor);
    if (descriptor == gTemporary)
    {
        ++gTemporaryCloses;
        if (gFault == Fault::ReusedClose)
        {
            gReused = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            return Error(EINTR);
        }
        if (gFault == Fault::TemporaryClose)
        {
            return Error();
        }
    }
    if (descriptor == gParent && gFault == Fault::ParentClose)
    {
        return Error();
    }
    return result;
}
} // namespace ludus::foundation::filesystem::persistence_test

TEST_CASE("Publication stage failures preserve old bytes and clean owned temporaries")
{
    for (const auto fault : {Fault::ParentOpen,
                             Fault::AncestorOpen,
                             Fault::TemporaryOpen,
                             Fault::FirstMetadata,
                             Fault::SecondMetadata,
                             Fault::PartialWrite,
                             Fault::ZeroWrite,
                             Fault::FileSync,
                             Fault::TemporaryClose,
                             Fault::Replace})
    {
        CAPTURE(static_cast<int>(fault));
        Reset(Fault::None);
        Fixture fixture;
        fixture.Write("nested/data", "old");
        WriteDirectory writer;
        REQUIRE(writer.Open(fixture.Root).Succeeded());
        Reset(fault);
        const auto result = Publish(writer, "nested/data", "replacement");
        REQUIRE(result.Outcome.Code == Status::IoError);
        REQUIRE_FALSE(result.Published);
        REQUIRE(result.Cleanup.Succeeded());
        REQUIRE(fixture.Read("nested/data") == "old");
        REQUIRE(fixture.Temporaries() == 0);
        if (gTemporary >= 0)
        {
            REQUIRE(gTemporaryCloses == 1);
        }
        Reset(Fault::None);
        REQUIRE(Publish(writer, "nested/data", "retry").Published);
    }
    Reset(Fault::None);
}
TEST_CASE("Publication retries interrupted operations and handles repeated short writes")
{
    for (const auto fault : {Fault::InterruptedOpen,
                             Fault::InterruptedMetadata,
                             Fault::InterruptedLock,
                             Fault::InterruptedWrite,
                             Fault::ShortWrite,
                             Fault::InterruptedSync})
    {
        CAPTURE(static_cast<int>(fault));
        Reset(Fault::None);
        Fixture fixture;
        WriteDirectory writer;
        REQUIRE(writer.Open(fixture.Root).Succeeded());
        Reset(fault);
        const auto result = Publish(writer, "nested/data", "all-bytes");
        REQUIRE(result.Outcome.Succeeded());
        REQUIRE(result.Published);
        REQUIRE(result.FileSynced);
        REQUIRE(result.DirectorySynced);
        REQUIRE(result.Cleanup.Succeeded());
        REQUIRE(gTemporaryCloses == 1);
        REQUIRE(fixture.Read("nested/data") == "all-bytes");
        REQUIRE(fixture.Temporaries() == 0);
    }
    Reset(Fault::None);
}
TEST_CASE("Post-publication directory sync failure reports visible bytes without rollback")
{
    Reset(Fault::None);
    Fixture fixture;
    fixture.Write("nested/data", "old");
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    Reset(Fault::DirectorySync);
    const auto result = Publish(writer, "nested/data", "visible");
    REQUIRE(result.Outcome.Code == Status::IoError);
    REQUIRE(result.Outcome.NativeCode == EIO);
    REQUIRE(result.Published);
    REQUIRE(result.FileSynced);
    REQUIRE_FALSE(result.DirectorySynced);
    REQUIRE(result.Cleanup.Succeeded());
    REQUIRE(fixture.Read("nested/data") == "visible");
    REQUIRE(fixture.Temporaries() == 0);
    Reset(Fault::None);
}
TEST_CASE("Cleanup failures and foreign temporary collisions remain observable")
{
    Reset(Fault::None);
    Fixture fixture;
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    Reset(Fault::Cleanup);
    const auto failedCleanup = Publish(writer, "nested/data", "x");
    REQUIRE(failedCleanup.Outcome.Code == Status::IoError);
    REQUIRE_FALSE(failedCleanup.Published);
    REQUIRE(failedCleanup.Cleanup.Code == Status::IoError);
    REQUIRE(fixture.Temporaries() == 1);
    for (const auto& entry : std::filesystem::directory_iterator(fixture.Path("nested")))
    {
        std::filesystem::remove(entry.path());
    }
    Reset(Fault::Collisions);
    const auto colliding = Publish(writer, "nested/data", "x");
    REQUIRE(colliding.Outcome.Code == Status::LimitExceeded);
    REQUIRE_FALSE(colliding.Published);
    REQUIRE(gCollisionCount == 16);
    REQUIRE(fixture.Temporaries() == 16);
    for (const auto& entry : std::filesystem::directory_iterator(fixture.Path("nested")))
    {
        REQUIRE(fixture.Read(std::string("nested/") + entry.path().filename().string()) == "foreign");
    }
    Reset(Fault::ParentClose);
    const auto closed = Publish(writer, "nested/data", "published");
    REQUIRE(closed.Published);
    REQUIRE(closed.Outcome.Succeeded());
    REQUIRE(closed.Cleanup.Code == Status::IoError);
    Reset(Fault::None);
}
TEST_CASE("Destination rechecks reject uncooperative changes observed before rename")
{
    for (const auto fault : {Fault::ExternalEdit, Fault::ExternalCreate})
    {
        Reset(Fault::None);
        Fixture fixture;
        WriteDirectory writer;
        Directory root;
        REQUIRE(writer.Open(fixture.Root).Succeeded());
        REQUIRE(root.Open(fixture.Root).Succeeded());
        WriteOptions options;
        options.Condition = WriteCondition::Missing;
        if (fault == Fault::ExternalEdit)
        {
            fixture.Write("nested/data", "old");
            options.Condition = WriteCondition::MatchingStamp;
            REQUIRE(root.Observe("nested/data", options.Expected).Succeeded());
        }
        Reset(fault);
        const auto result = Publish(writer, "nested/data", "prepared", options);
        REQUIRE(result.Outcome.Code == Status::Conflict);
        REQUIRE_FALSE(result.Published);
        REQUIRE(fixture.Read("nested/data") == "external-change");
        REQUIRE(fixture.Temporaries() == 0);
    }
    Reset(Fault::None);
}
TEST_CASE("Root failure preserves capability and concurrent same-owner publication holds independent locks")
{
    Reset(Fault::None);
    Fixture fixture;
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    for (const auto fault : {Fault::Allocation, Fault::RootOpen})
    {
        Reset(fault);
        const auto result = writer.Open(fixture.Root);
        REQUIRE(result.Code == (fault == Fault::Allocation ? Status::OutOfMemory : Status::IoError));
        REQUIRE(writer.IsOpen());
    }
    Reset(Fault::Lock);
    REQUIRE(Publish(writer, "nested/data", "x").Outcome.Code == Status::Conflict);
    Reset(Fault::Reentrant);
    gWriter = &writer;
    const auto result = Publish(writer, "nested/data", "outer");
    REQUIRE(result.Published);
    REQUIRE(gReentrant == Status::Conflict);
    REQUIRE_FALSE(std::filesystem::exists(fixture.Path("nested/second")));
    Reset(Fault::None);
}

TEST_CASE("Publication never retries close after an interrupted close reuses the descriptor")
{
    Reset(Fault::None);
    Fixture fixture;
    fixture.Write("nested/data", "old");
    WriteDirectory writer;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    Reset(Fault::ReusedClose);
    const auto result = Publish(writer, "nested/data", "prepared");
    REQUIRE(result.Outcome.Code == Status::IoError);
    REQUIRE(result.Outcome.NativeCode == EINTR);
    REQUIRE_FALSE(result.Published);
    REQUIRE(gTemporaryCloses == 1);
    REQUIRE(gReused == gTemporary);
    REQUIRE(::fcntl(gReused, F_GETFD) >= 0);
    REQUIRE(fixture.Read("nested/data") == "old");
    REQUIRE(fixture.Temporaries() == 0);
    Reset(Fault::None);
}
