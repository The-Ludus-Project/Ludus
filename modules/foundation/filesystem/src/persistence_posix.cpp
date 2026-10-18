#include <ludus/foundation/filesystem/persistence.hpp>

#include "internal/posix_metadata.hpp"
#if defined(LUDUS_FILESYSTEM_PERSISTENCE_FAULT_TESTING)
#    include "internal/persistence_test_hooks.hpp"
#endif

#include <atomic>
#include <new>
#include <span>
#include <string_view>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(LUDUS_PLATFORM_LINUX)
#    include <sys/syscall.h>
#else
#    include <sys/stdio.h>
#endif

// Thanks to the Linux man-pages project, rename(2), fsync(2), and flock(2):
// atomic replacement retains opened revisions; directory sync is separate;
// advisory locks coordinate only cooperating writers. Independently implemented.
// https://man7.org/linux/man-pages/man2/rename.2.html
// https://man7.org/linux/man-pages/man2/fsync.2.html
// https://man7.org/linux/man-pages/man2/flock.2.html
// Thanks to Apple, fsync(2), for distinguishing native acknowledgment from
// device-cache persistence. This baseline uses fsync, not F_FULLFSYNC.
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html
// See docs/architecture/filesystem.md for the trust and durability boundary.

namespace ludus::foundation::filesystem
{
namespace
{
#if defined(LUDUS_FILESYSTEM_PERSISTENCE_FAULT_TESTING)
namespace native = persistence_test;
#else
namespace native
{
int OpenAt(int parent, const char* name, int flags, mode_t mode) noexcept
{
    return ::openat(parent, name, flags, mode);
}
int MetadataAt(int parent, const char* name, struct stat* info, int flags) noexcept
{
    return ::fstatat(parent, name, info, flags);
}
int Lock(int descriptor, int operation) noexcept
{
    return ::flock(descriptor, operation);
}
isize Write(int descriptor, std::span<const uint8> bytes) noexcept
{
    return ::write(descriptor, bytes.data(), bytes.size());
}
int Sync(int descriptor) noexcept
{
    return ::fsync(descriptor);
}
int Replace(int parent, const char* source, const char* target) noexcept
{
#    if defined(LUDUS_PLATFORM_LINUX)
    return static_cast<int>(::syscall(SYS_renameat, parent, source, parent, target));
#    else
    return ::renameat(parent, source, parent, target);
#    endif
}
int Unlink(int parent, const char* name) noexcept
{
    return ::unlinkat(parent, name, 0);
}
int Close(int descriptor) noexcept
{
    return ::close(descriptor);
}
} // namespace native
#endif
Result Failure(int error) noexcept
{
    Status code = Status::IoError;
    if (error == ENOENT)
    {
        code = Status::NotFound;
    }
    else if (error == EACCES || error == EPERM || error == ELOOP)
    {
        code = Status::AccessDenied;
    }
    else if (error == ENOTDIR || error == ENAMETOOLONG)
    {
        code = Status::InvalidArgument;
    }
    return {code, static_cast<int32>(error)};
}
void Copy(std::string_view value, char* output) noexcept
{
    for (usize i = 0; i < value.size(); ++i)
    {
        output[i] = value[i];
    }
    output[value.size()] = '\0';
}
int Open(int parent, const char* name, int flags, mode_t mode = 0) noexcept
{
    int value;
    do
    {
        value = native::OpenAt(parent, name, flags, mode);
    } while (value < 0 && errno == EINTR);
    return value;
}
Result Close(int& descriptor) noexcept
{
    const int value = descriptor;
    descriptor = -1;
    // Linux/Darwin consume ownership even when close reports an error.
    return value >= 0 && native::Close(value) != 0 ? Failure(errno) : Result{};
}
Result Sync(int descriptor) noexcept
{
    int value;
    do
    {
        value = native::Sync(descriptor);
    } while (value < 0 && errno == EINTR);
    return value == 0 ? Result{} : Failure(errno);
}
Result Check(int parent, const char* name, const WriteOptions& options) noexcept
{
    struct stat info{};
    int value;
    do
    {
        value = native::MetadataAt(parent, name, &info, AT_SYMLINK_NOFOLLOW);
    } while (value < 0 && errno == EINTR);
    if (value < 0)
    {
        if (errno == ENOENT)
        {
            return options.Condition == WriteCondition::MatchingStamp ? Result{Status::Conflict} : Result{};
        }
        return Failure(errno);
    }
    if (S_ISLNK(info.st_mode))
    {
        return {Status::AccessDenied};
    }
    if (S_ISREG(info.st_mode) == false || info.st_size < 0)
    {
        return {Status::NotRegularFile};
    }
    if (options.Condition == WriteCondition::Missing ||
        (options.Condition == WriteCondition::MatchingStamp && detail::Stamp(info) != options.Expected))
    {
        return {Status::Conflict};
    }
    return {};
}
struct Work final
{
    int Parent = -1;
    int Temporary = -1;
    char Leaf[MAX_PATH_BYTES + 1]{};
    char Name[64]{};
    bool OwnsTemporary = false;
    PublicationResult Finish(PublicationResult result) noexcept
    {
        const auto temporaryClose = Close(Temporary);
        if (result.Cleanup.Succeeded())
        {
            result.Cleanup = temporaryClose;
        }
        if (OwnsTemporary && native::Unlink(Parent, Name) != 0 && result.Cleanup.Succeeded())
        {
            result.Cleanup = Failure(errno);
        }
        const auto closed = Close(Parent);
        if (result.Cleanup.Succeeded())
        {
            result.Cleanup = closed;
        }
        return result;
    }
};
Result Parent(int root, std::string_view path, Work& work) noexcept
{
    // dup would share the flock open-file description with simultaneous calls.
    // Reopen '.' so every transaction holds an independent cooperative lock.
    work.Parent = Open(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (work.Parent < 0)
    {
        return Failure(errno);
    }
    usize begin = 0;
    while (true)
    {
        const auto slash = path.find('/', begin);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        Copy(path.substr(begin, end - begin), work.Leaf);
        if (slash == std::string_view::npos)
        {
            return {};
        }
        const int next = Open(work.Parent, work.Leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            return Failure(errno);
        }
        const auto closed = Close(work.Parent);
        work.Parent = next;
        if (closed.Succeeded() == false)
        {
            return closed;
        }
        begin = slash + 1;
    }
}
std::atomic<uint64> gSequence{1};
Result Temporary(Work& work) noexcept
{
    for (usize attempt = 0; attempt < 16; ++attempt)
    {
        uint64 sequence = gSequence.load(std::memory_order_relaxed);
        do
        {
            if (sequence == ~uint64{0})
            {
                return {Status::LimitExceeded};
            }
        } while (gSequence.compare_exchange_weak(sequence, sequence + 1, std::memory_order_relaxed) == false);
        constexpr std::string_view PREFIX = ".ludus-write-";
        Copy(PREFIX, work.Name);
        usize position = PREFIX.size();
        constexpr char HEX[] = "0123456789abcdef";
        const uint64 numbers[]{static_cast<uint64>(::getpid()), sequence};
        for (uint64 number : numbers)
        {
            for (usize digit = 0; digit < 16; ++digit)
            {
                const auto shift = static_cast<uint32>((15 - digit) * 4);
                work.Name[position++] = HEX[(number >> shift) & 15];
            }
            work.Name[position++] = '-';
        }
        work.Name[position - 1] = '\0';
        // Never replace or remove a colliding foreign temporary.
        if (std::string_view(work.Name) == work.Leaf)
        {
            continue;
        }
        work.Temporary = Open(work.Parent, work.Name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (work.Temporary >= 0)
        {
            work.OwnsTemporary = true;
            return {};
        }
        if (errno != EEXIST)
        {
            return Failure(errno);
        }
    }
    return {Status::LimitExceeded};
}
} // namespace
struct WriteDirectory::Impl final
{
    int Root = -1;
};
WriteDirectory::~WriteDirectory() noexcept
{
    (void)Close();
}
WriteDirectory::WriteDirectory(WriteDirectory&& other) noexcept : mImpl(other.mImpl)
{
    other.mImpl = nullptr;
}
WriteDirectory& WriteDirectory::operator=(WriteDirectory&& other) noexcept
{
    if (this != &other)
    {
        (void)Close();
        mImpl = other.mImpl;
        other.mImpl = nullptr;
    }
    return *this;
}
bool WriteDirectory::IsOpen() const noexcept
{
    return mImpl != nullptr;
}
Result WriteDirectory::Open(std::string_view nativeRoot) noexcept
{
    if (nativeRoot.empty() || nativeRoot.size() > MAX_ROOT_BYTES || nativeRoot.find('\0') != std::string_view::npos)
    {
        return {Status::InvalidArgument};
    }
#if defined(LUDUS_FILESYSTEM_PERSISTENCE_FAULT_TESTING)
    if (persistence_test::AllocationAllowed() == false)
    {
        return {Status::OutOfMemory};
    }
#endif
    auto* next = new (std::nothrow) Impl();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    char name[MAX_ROOT_BYTES + 1];
    Copy(nativeRoot, name);
    next->Root = filesystem::Open(AT_FDCWD, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (next->Root < 0)
    {
        const auto result = Failure(errno);
        delete next;
        return result;
    }
    (void)Close();
    mImpl = next;
    return {};
}
Result WriteDirectory::Close() noexcept
{
    if (mImpl == nullptr)
    {
        return {};
    }
    const auto result = filesystem::Close(mImpl->Root);
    delete mImpl;
    mImpl = nullptr;
    return result;
}
PublicationResult WriteDirectory::Publish(std::string_view relativePath,
                                          std::span<const uint8> bytes,
                                          const WriteOptions& options) const noexcept
{
    if (mImpl == nullptr || ValidPath(relativePath) == false ||
        (options.Sync != SyncPolicy::PublishOnly && options.Sync != SyncPolicy::File &&
         options.Sync != SyncPolicy::FileAndDirectory) ||
        (options.Condition != WriteCondition::Any && options.Condition != WriteCondition::Missing &&
         options.Condition != WriteCondition::MatchingStamp))
    {
        return {{Status::InvalidArgument}};
    }
    if (bytes.size() > options.MaxBytes)
    {
        return {{Status::LimitExceeded}};
    }
    Work work;
    PublicationResult result;
    result.Outcome = Parent(mImpl->Root, relativePath, work);
    if (result.Outcome.Succeeded() == false)
    {
        return work.Finish(result);
    }
    int locked;
    do
    {
        locked = native::Lock(work.Parent, LOCK_EX | LOCK_NB);
    } while (locked < 0 && errno == EINTR);
    if (locked < 0)
    {
        result.Outcome = errno == EWOULDBLOCK || errno == EAGAIN ? Result{Status::Conflict, errno} : Failure(errno);
        return work.Finish(result);
    }
    result.Outcome = Check(work.Parent, work.Leaf, options);
    if (result.Outcome.Succeeded())
    {
        result.Outcome = Temporary(work);
    }
    usize total = 0;
    while (result.Outcome.Succeeded() && total < bytes.size())
    {
        constexpr usize MAX_TRANSFER = 0x7ffff000;
        const usize left = bytes.size() - total;
        const usize count = left < MAX_TRANSFER ? left : MAX_TRANSFER;
        isize written;
        do
        {
            written = native::Write(work.Temporary, bytes.subspan(total, count));
        } while (written < 0 && errno == EINTR);
        if (written <= 0)
        {
            result.Outcome = written == 0 ? Result{Status::IoError} : Failure(errno);
            break;
        }
        total += static_cast<usize>(written);
    }
    if (result.Outcome.Succeeded() && options.Sync != SyncPolicy::PublishOnly)
    {
        result.Outcome = Sync(work.Temporary);
        result.FileSynced = result.Outcome.Succeeded();
    }
    const auto closed = filesystem::Close(work.Temporary);
    if (result.Outcome.Succeeded())
    {
        result.Outcome = closed;
    }
    else if (closed.Succeeded() == false)
    {
        result.Cleanup = closed;
    }
    if (result.Outcome.Succeeded())
    {
        result.Outcome = Check(work.Parent, work.Leaf, options);
    }
    if (result.Outcome.Succeeded())
    {
        if (native::Replace(work.Parent, work.Name, work.Leaf) != 0)
        {
            result.Outcome = Failure(errno);
        }
        else
        {
            work.OwnsTemporary = false;
            result.Published = true;
        }
    }
    if (result.Published && options.Sync == SyncPolicy::FileAndDirectory)
    {
        result.Outcome = Sync(work.Parent);
        result.DirectorySynced = result.Outcome.Succeeded();
    }
    return work.Finish(result);
}
} // namespace ludus::foundation::filesystem
