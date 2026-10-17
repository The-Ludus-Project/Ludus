#include <ludus/content/content.h>
#include <ludus/foundation/base/config.h>

#include "internal/save_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>

#include <fcntl.h>
#include <unistd.h>
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
#    include <sys/stdio.h>
#else
#    include <stdio.h>
#endif

using namespace ludus::content;
namespace
{
enum class Fault : uint8
{
    None,
    InterruptedWrite,
    ShortWrites,
    PartialError,
    ZeroWrite,
    InterruptedSync,
    FileSync,
    FileClose,
    Replace,
    DirectorySync,
    ExternalEdit,
    ExternalCreate,
};
Fault gFault = Fault::None;
usize gWrites = 0;
usize gSyncs = 0;
usize gReplaces = 0;
int gTemporary = -1;
usize gTemporaryCloses = 0;
const char* gRoot = nullptr;
constexpr uint8 ORIGINAL[] = {'a', 'b', 'c'};
constexpr uint8 REPLACEMENT[] = {'x', 'y', 'z', '!'};
struct Fixture final
{
    char Root[64] = "/tmp/ludus-content-save-fault-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Root) != nullptr);
        REQUIRE(SaveFile(Root, "data", ORIGINAL, nullptr) == Status::Ok);
        gWrites = 0;
        gSyncs = 0;
        gReplaces = 0;
        gTemporary = -1;
        gTemporaryCloses = 0;
        gRoot = Root;
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Fixture()
    {
        gFault = Fault::None;
        gRoot = nullptr;
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
    void Check(std::string_view expected) const
    {
        Bytes output;
        REQUIRE(ReadFile(Root, "data", 8, output) == Status::Ok);
        REQUIRE(output.String() == expected);
        REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(Root) / "data.ludus-save"));
    }
};
} // namespace

namespace ludus::content::test
{
isize Write(int descriptor, std::span<const uint8> data) noexcept
{
    gTemporary = descriptor;
    ++gWrites;
    if (gFault == Fault::InterruptedWrite && gWrites == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (gFault == Fault::PartialError && gWrites > 1)
    {
        errno = EIO;
        return -1;
    }
    if (gFault == Fault::ZeroWrite)
    {
        return 0;
    }
    if ((gFault == Fault::ExternalEdit || gFault == Fault::ExternalCreate) && gWrites == 1)
    {
        // Simulate an uncooperative edit after the first expected-digest check.
        const int directory = ::open(gRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const char* destination = gFault == Fault::ExternalCreate ? "new" : "data";
        const int current = ::openat(directory, destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const uint8 external[] = {'e', 'd', 'i', 't'};
        const auto written = ::write(current, external, sizeof(external));
        const int fileClosed = ::close(current);
        const int directoryClosed = ::close(directory);
        if (written != static_cast<isize>(sizeof(external)) || fileClosed != 0 || directoryClosed != 0)
        {
            errno = EIO;
            return -1;
        }
    }
    const auto count = gFault == Fault::ShortWrites || gFault == Fault::PartialError ? usize{1} : data.size();
    return ::write(descriptor, data.data(), count);
}
int Sync(int descriptor) noexcept
{
    ++gSyncs;
    if (gFault == Fault::InterruptedSync && gSyncs == 1)
    {
        errno = EINTR;
        return -1;
    }
    if ((gFault == Fault::FileSync && gSyncs == 1) || (gFault == Fault::DirectorySync && gSyncs == 2))
    {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}
int Replace(int directory, const char* source, const char* destination) noexcept
{
    ++gReplaces;
    if (gFault == Fault::Replace)
    {
        errno = EIO;
        return -1;
    }
    // Resolve the real operation against its descriptor, without a pathname
    // reopen. renameat is declared by stdio in Linux test translation units.
    return ::renameat(directory, source, directory, destination);
}
int Close(int descriptor) noexcept
{
    const int result = ::close(descriptor);
    if (descriptor == gTemporary)
    {
        ++gTemporaryCloses;
        gTemporary = -1;
        if (gFault == Fault::FileClose)
        {
            // Darwin/Linux release ownership even when reporting a close error.
            errno = EIO;
            return -1;
        }
    }
    return result;
}
} // namespace ludus::content::test

TEST_CASE("Native saves retry interrupted writes and sync and finish short writes", "[content][save][fault]")
{
    for (const auto fault : {Fault::InterruptedWrite, Fault::ShortWrites, Fault::InterruptedSync})
    {
        Fixture fixture;
        const auto digest = Hash(ORIGINAL);
        gFault = fault;
        REQUIRE(SaveFile(fixture.Root, "data", REPLACEMENT, &digest) == Status::Ok);
        REQUIRE(gReplaces == 1);
        REQUIRE(gTemporaryCloses == 1);
        fixture.Check("xyz!");
    }
}

TEST_CASE("Native save failures before publication preserve original data and clean up", "[content][save][fault]")
{
    for (const auto fault : {Fault::PartialError, Fault::ZeroWrite, Fault::FileSync, Fault::FileClose, Fault::Replace})
    {
        Fixture fixture;
        const auto digest = Hash(ORIGINAL);
        gFault = fault;
        REQUIRE(SaveFile(fixture.Root, "data", REPLACEMENT, &digest) == Status::IoError);
        REQUIRE(gTemporaryCloses == 1);
        REQUIRE(gReplaces == (fault == Fault::Replace ? 1 : 0));
        fixture.Check("abc");
        gFault = Fault::None;
        REQUIRE(SaveFile(fixture.Root, "data", REPLACEMENT, &digest) == Status::Ok);
        fixture.Check("xyz!");
    }
}

TEST_CASE("Native saves recheck the destination after writing and preserve an external edit", "[content][save][fault]")
{
    Fixture fixture;
    const auto digest = Hash(ORIGINAL);
    gFault = Fault::ExternalEdit;
    REQUIRE(SaveFile(fixture.Root, "data", REPLACEMENT, &digest) == Status::Conflict);
    REQUIRE(gReplaces == 0);
    REQUIRE(gTemporaryCloses == 1);
    fixture.Check("edit");
}

TEST_CASE("Directory sync failure still reports completed atomic publication", "[content][save][fault]")
{
    Fixture fixture;
    const auto digest = Hash(ORIGINAL);
    gFault = Fault::DirectorySync;
    REQUIRE(SaveFile(fixture.Root, "data", REPLACEMENT, &digest) == Status::Ok);
    REQUIRE(gSyncs == 2);
    REQUIRE(gReplaces == 1);
    fixture.Check("xyz!");
}

TEST_CASE("Native create rechecks absence and preserves a file created while staging", "[content][save][fault]")
{
    Fixture fixture;
    gFault = Fault::ExternalCreate;
    REQUIRE(SaveFile(fixture.Root, "new", REPLACEMENT, nullptr) == Status::Conflict);
    REQUIRE(gReplaces == 0);
    Bytes output;
    REQUIRE(ReadFile(fixture.Root, "new", 8, output) == Status::Ok);
    REQUIRE(output.String() == "edit");
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(fixture.Root) / "new.ludus-save"));
}

TEST_CASE("Failed native creates leave no destination or owned temporary", "[content][save][fault]")
{
    for (const auto fault : {Fault::PartialError, Fault::ZeroWrite, Fault::FileSync, Fault::FileClose, Fault::Replace})
    {
        Fixture fixture;
        gFault = fault;
        REQUIRE(SaveFile(fixture.Root, "new", REPLACEMENT, nullptr) == Status::IoError);
        REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(fixture.Root) / "new"));
        REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(fixture.Root) / "new.ludus-save"));
        gFault = Fault::None;
        REQUIRE(SaveFile(fixture.Root, "new", REPLACEMENT, nullptr) == Status::Ok);
    }
}
