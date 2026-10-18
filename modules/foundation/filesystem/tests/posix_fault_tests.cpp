#include <ludus/foundation/filesystem/filesystem.hpp>

#include "internal/posix_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>

#include <fcntl.h>
#include <unistd.h>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
enum class ReadFault : uint8
{
    None,
    Interrupt,
    Short,
    PartialError,
    Mutate,
};
ReadFault gReadFault = ReadFault::None;
usize gReadCalls = 0;
usize gAllocations = 0;
usize gCloseCalls = 0;
bool gFailAllocation = false;
bool gInterruptOpen = false;
bool gInterruptDuplicate = false;
bool gInterruptMetadata = false;
bool gFailMetadata = false;
bool gInterruptClose = false;
int gLastOpened = -1;
int gLastDuplicate = -1;
int gWriter = -1;
int gReused = -1;
struct Fixture final
{
    char Path[64] = "/tmp/ludus-fs-darwin-fault-XXXXXX";
    Fixture()
    {
        REQUIRE(::mkdtemp(Path) != nullptr);
        std::ofstream file(std::filesystem::path(Path) / "data", std::ios::binary);
        file << "abcdef";
        file.close();
        REQUIRE(file.good());
    }
    // Best-effort temporary-directory cleanup: only std::filesystem::path allocation
    // can throw, and bad_alloc terminating the test process is acceptable here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Fixture()
    {
        gReadFault = ReadFault::None;
        gFailAllocation = false;
        gFailMetadata = false;
        gInterruptClose = false;
        if (gWriter >= 0)
        {
            (void)::close(gWriter);
            gWriter = -1;
        }
        if (gReused >= 0)
        {
            (void)::close(gReused);
            gReused = -1;
        }
        std::error_code ignored;
        std::filesystem::remove_all(Path, ignored);
    }
};
usize DescriptorCount()
{
    usize count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/dev/fd"))
    {
        ++count;
    }
    return count;
}
} // namespace
namespace ludus::foundation::filesystem::test
{
int OpenAt(int parent, const char* name, int flags) noexcept
{
    if (gInterruptOpen)
    {
        gInterruptOpen = false;
        errno = EINTR;
        return -1;
    }
    gLastOpened = ::openat(parent, name, flags);
    return gLastOpened;
}
int Duplicate(int descriptor) noexcept
{
    if (gInterruptDuplicate)
    {
        gInterruptDuplicate = false;
        errno = EINTR;
        return -1;
    }
    gLastDuplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    return gLastDuplicate;
}
int Metadata(int descriptor, struct stat* info) noexcept
{
    if (gInterruptMetadata || gFailMetadata)
    {
        errno = gInterruptMetadata ? EINTR : EIO;
        gInterruptMetadata = false;
        return -1;
    }
    return ::fstat(descriptor, info);
}
isize Read(int descriptor, std::span<uint8> buffer, off_t offset) noexcept
{
    ++gReadCalls;
    if (gReadFault == ReadFault::Interrupt && gReadCalls == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (gReadFault == ReadFault::PartialError && gReadCalls == 2)
    {
        errno = EIO;
        return -1;
    }
    const usize count = buffer.size();
    const usize actual =
        (gReadFault == ReadFault::Short || gReadFault == ReadFault::PartialError) && count > 2 ? 2 : count;
    const auto received = ::pread(descriptor, buffer.data(), actual, offset);
    if (gReadFault == ReadFault::Mutate && ::ftruncate(gWriter, 1) != 0)
    {
        return -1;
    }
    return received;
}
int Close(int descriptor) noexcept
{
    ++gCloseCalls;
    const auto result = ::close(descriptor);
    if (gInterruptClose && result == 0)
    {
        gInterruptClose = false;
        // Model Darwin's released descriptor on error, with immediate reuse.
        const int replacement = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (replacement >= 0)
        {
            gReused = replacement == descriptor ? replacement : ::dup2(replacement, descriptor);
            if (replacement != descriptor)
            {
                (void)::close(replacement);
            }
        }
        errno = EINTR;
        return -1;
    }
    return result;
}
bool AllocationAllowed() noexcept
{
    ++gAllocations;
    return gFailAllocation == false;
}
} // namespace ludus::foundation::filesystem::test
TEST_CASE("Darwin retries interrupted opens clones metadata and reads with close-on-exec handles")
{
    Fixture fixture;
    Directory root;
    File file, clone;
    gInterruptOpen = true;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE((::fcntl(gLastOpened, F_GETFD) & FD_CLOEXEC) != 0);
    gInterruptOpen = true;
    gInterruptMetadata = true;
    REQUIRE(root.OpenRead("data", file).Succeeded());
    REQUIRE((::fcntl(gLastOpened, F_GETFD) & FD_CLOEXEC) != 0);
    gInterruptDuplicate = true;
    REQUIRE(file.Clone(clone).Succeeded());
    REQUIRE((::fcntl(gLastDuplicate, F_GETFD) & FD_CLOEXEC) != 0);
    uint8 bytes[6]{};
    for (const auto fault : {ReadFault::Interrupt, ReadFault::Short})
    {
        gReadFault = fault;
        gReadCalls = 0;
        gInterruptMetadata = true;
        const auto read = file.ReadAt(0, bytes);
        gReadFault = ReadFault::None;
        REQUIRE(read.Outcome.Succeeded());
        REQUIRE(read.BytesRead == 6);
        REQUIRE(gReadCalls > 1);
        REQUIRE(bytes[0] == 'a');
        REQUIRE(bytes[5] == 'f');
    }
}
TEST_CASE("Darwin read faults report partial progress and discard revisions changed during a read")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("data", file).Succeeded());
    uint8 bytes[6]{};
    gReadFault = ReadFault::PartialError;
    gReadCalls = 0;
    const auto partial = file.ReadAt(0, bytes);
    gReadFault = ReadFault::None;
    REQUIRE(partial.Outcome.Code == Status::IoError);
    REQUIRE(partial.Outcome.NativeCode == EIO);
    REQUIRE(partial.BytesRead == 2);
    gWriter = ::open((std::filesystem::path(fixture.Path) / "data").c_str(), O_WRONLY | O_CLOEXEC);
    REQUIRE(gWriter >= 0);
    gReadFault = ReadFault::Mutate;
    const auto changed = file.ReadAt(0, bytes);
    gReadFault = ReadFault::None;
    REQUIRE(changed.Outcome.Code == Status::Changed);
    REQUIRE(changed.BytesRead == 0);
}
TEST_CASE("Darwin failed allocations and metadata preserve owners and release staged descriptors")
{
    Fixture fixture;
    Directory root;
    File file, clone;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("data", file).Succeeded());
    REQUIRE(file.Clone(clone).Succeeded());
    const auto before = DescriptorCount();
    gFailAllocation = true;
    const auto reopen = root.Open("/tmp");
    const auto open = root.OpenRead("data", file);
    const auto duplicate = file.Clone(clone);
    gFailAllocation = false;
    REQUIRE(reopen.Code == Status::OutOfMemory);
    REQUIRE(open.Code == Status::OutOfMemory);
    REQUIRE(duplicate.Code == Status::OutOfMemory);
    gFailMetadata = true;
    const auto metadata = root.OpenRead("data", file);
    gFailMetadata = false;
    REQUIRE(metadata.Code == Status::IoError);
    REQUIRE(metadata.NativeCode == EIO);
    REQUIRE(root.IsOpen());
    REQUIRE(file.Size() == 6);
    REQUIRE(clone.Size() == 6);
    REQUIRE(DescriptorCount() == before);
    const auto allocations = gAllocations;
    uint8 bytes[6]{};
    const auto read = file.ReadAt(0, bytes);
    REQUIRE(ValidPath("nested/音.wav"));
    REQUIRE(read.Outcome.Succeeded());
    REQUIRE(gAllocations == allocations);
}
TEST_CASE("Darwin close errors empty owners without retrying a reused descriptor")
{
    Fixture fixture;
    Directory root;
    File file;
    REQUIRE(root.Open(fixture.Path).Succeeded());
    REQUIRE(root.OpenRead("data", file).Succeeded());
    gCloseCalls = 0;
    gInterruptClose = true;
    const auto result = file.Close();
    REQUIRE(result.Code == Status::IoError);
    REQUIRE(result.NativeCode == EINTR);
    REQUIRE_FALSE(file.IsOpen());
    REQUIRE(file.Close().Succeeded());
    REQUIRE(gCloseCalls == 1);
    REQUIRE(gReused >= 0);
    REQUIRE(::fcntl(gReused, F_GETFD) >= 0);
    REQUIRE(::close(gReused) == 0);
    gReused = -1;
    gCloseCalls = 0;
    gInterruptClose = true;
    REQUIRE(root.Close().NativeCode == EINTR);
    REQUIRE_FALSE(root.IsOpen());
    REQUIRE(root.Close().Succeeded());
    REQUIRE(gCloseCalls == 1);
    REQUIRE(gReused >= 0);
    REQUIRE(::fcntl(gReused, F_GETFD) >= 0);
}
