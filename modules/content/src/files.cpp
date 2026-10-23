#include <ludus/content/content.h>
#include <ludus/content/limits.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <new>
#include <span>
#include <string_view>

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_WEB
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/file.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    if LUDUS_TARGET_OS == LUDUS_OS_MACOS
// Darwin declares descriptor-relative rename in this native header, without
// bringing in C stdio or diagnostic APIs.
#        include <sys/stdio.h>
#    elif LUDUS_TARGET_OS == LUDUS_OS_WEB
// Emscripten declares renameat here; no stdio diagnostics are used.
#        include <stdio.h>
#    else
#        include <sys/syscall.h>
#    endif
#    if defined(LUDUS_CONTENT_SAVE_FAULT_TESTING)
#        include "internal/save_test_hooks.hpp"
#    endif
#endif

namespace ludus::content
{
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_WEB
namespace
{
// Thanks to Apple, Mac OS X Manual Pages, flock(2), rename(2) and fsync(2):
// advisory ownership, atomic pathname replacement and the distinction between
// fsync and drive durability inform this save adapter. Independently implemented;
// no manual code copied. See docs/architecture/content-resources.md, "Native saves".
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/rename.2.html
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html
#    if defined(LUDUS_CONTENT_SAVE_FAULT_TESTING)
namespace native = test;
#    else
namespace native
{
isize Write(int descriptor, std::span<const uint8> data) noexcept
{
    return ::write(descriptor, data.data(), data.size());
}
int Sync(int descriptor) noexcept
{
    return ::fsync(descriptor);
}
int Replace(int directory, const char* source, const char* destination) noexcept
{
#        if LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_WEB
    return ::renameat(directory, source, directory, destination);
#        else
    return static_cast<int>(::syscall(SYS_renameat, directory, source, directory, destination));
#        endif
}
int Close(int descriptor) noexcept
{
    return ::close(descriptor);
}
} // namespace native
#    endif
int OpenAt(int directory, const char* name, int flags, mode_t mode = 0) noexcept
{
    int result;
    do
    {
        result = ::openat(directory, name, flags, mode);
    } while (result < 0 && errno == EINTR);
    return result;
}
int Sync(int descriptor) noexcept
{
    int result;
    do
    {
        result = native::Sync(descriptor);
    } while (result < 0 && errno == EINTR);
    return result;
}
isize Read(int descriptor, std::span<uint8> bytes) noexcept
{
    isize result;
    do
    {
        result = ::read(descriptor, bytes.data(), bytes.size());
    } while (result < 0 && errno == EINTR);
    return result;
}
struct Fd final
{
    int Value = -1;
    Fd() noexcept = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() noexcept
    {
        (void)Close();
    }
    [[nodiscard]] bool Close() noexcept
    {
        const int descriptor = Value;
        Value = -1;
        // Linux and Darwin release ownership even on close errors; never retry.
        return descriptor < 0 || native::Close(descriptor) == 0;
    }
};
Status Parent(std::string_view root, std::string_view path, Fd& directory, Text<1024>& leaf) noexcept
{
    if (root.empty() || root.size() > MAX_ROOT_BYTES || root.find('\0') != std::string_view::npos ||
        ValidPath(path) == false)
    {
        return Status::Invalid;
    }
    Text<4096> name;
    (void)name.Set(root);
    directory.Value = OpenAt(AT_FDCWD, name.Data, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory.Value < 0)
    {
        return Status::IoError;
    }
    usize begin = 0;
    while (true)
    {
        const auto slash = path.find('/', begin);
        if (slash == std::string_view::npos)
        {
            (void)leaf.Set(path.substr(begin));
            return Status::Ok;
        }
        Text<1024> segment;
        (void)segment.Set(path.substr(begin, slash - begin));
        const int next = OpenAt(directory.Value, segment.Data, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            return Status::IoError;
        }
        (void)directory.Close();
        directory.Value = next;
        begin = slash + 1;
    }
}
Status ReadAt(int directory, const char* path, usize cap, Bytes& output) noexcept
{
    Fd fd;
    fd.Value = OpenAt(directory, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd.Value < 0)
    {
        return errno == ENOENT ? Status::NotFound : Status::IoError;
    }
    struct stat info{};
    if (fstat(fd.Value, &info) != 0 || S_ISREG(info.st_mode) == false || info.st_size < 0)
    {
        return Status::IoError;
    }
    usize size{};
    if (ludus::foundation::TryIntegerCast(info.st_size, size) == false || size > cap)
    {
        return Status::Limit;
    }
    Bytes next;
    if (next.Resize(size) == false)
    {
        return Status::OutOfMemory;
    }
    usize offset = 0;
    while (offset < size)
    {
        const auto count = Read(fd.Value, next.Data().subspan(offset));
        if (count <= 0)
        {
            return Status::IoError;
        }
        offset += static_cast<usize>(count);
    }
    uint8 extra = 0;
    const auto count = Read(fd.Value, {&extra, 1});
    if (count != 0)
    {
        return count < 0 ? Status::IoError : Status::Conflict;
    }
    output = Move(next);
    return Status::Ok;
}
} // namespace
#endif
namespace
{
Status ContentStatus(foundation::filesystem::Status status) noexcept
{
    using FileStatus = foundation::filesystem::Status;
    switch (status)
    {
        case FileStatus::Ok:
            return Status::Ok;
        case FileStatus::CorruptData:
        case FileStatus::InvalidArgument:
            return Status::Invalid;
        case FileStatus::NotFound:
            return Status::NotFound;
        case FileStatus::LimitExceeded:
            return Status::Limit;
        case FileStatus::OutOfMemory:
            return Status::OutOfMemory;
        case FileStatus::Conflict:
        case FileStatus::Changed:
            return Status::Conflict;
        case FileStatus::Unsupported:
            return Status::Unsupported;
        case FileStatus::AccessDenied:
        case FileStatus::NotRegularFile:
        case FileStatus::IoError:
            return Status::IoError;
    }
    return Status::IoError;
}
} // namespace
Status ReadFile(std::string_view root, std::string_view path, usize cap, Bytes& output) noexcept
{
    if (root.empty() || ValidPath(path) == false)
    {
        return Status::Invalid;
    }
    foundation::filesystem::Directory directory;
    auto result = directory.Open(root);
    if (result.Succeeded() == false)
    {
        return ContentStatus(result.Code);
    }
    foundation::filesystem::File file;
    result = directory.OpenRead(path, file);
    if (result.Succeeded() == false)
    {
        return ContentStatus(result.Code);
    }
    if (file.Size() > cap)
    {
        return Status::Limit;
    }
    Bytes next;
    usize size = 0;
    if (foundation::TryIntegerCast(file.Size(), size) == false)
    {
        return Status::Limit;
    }
    if (next.Resize(size) == false)
    {
        return Status::OutOfMemory;
    }
    const auto read = file.ReadAt(0, next.Data());
    if (read.Outcome.Succeeded() == false)
    {
        return ContentStatus(read.Outcome.Code);
    }
    output = Move(next);
    return Status::Ok;
}
Status
SaveFile(std::string_view root, std::string_view path, std::span<const uint8> data, const Digest* expected) noexcept
{
    if (root.empty() || ValidPath(path) == false)
    {
        return Status::Invalid;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_WEB
    if (data.size() > MAX_SAVE_BYTES)
    {
        return Status::Limit;
    }
    Fd directory;
    Text<1024> leaf;
    auto status = Parent(root, path, directory, leaf);
    if (status != Status::Ok)
    {
        return status;
    }
#    if LUDUS_TARGET_OS != LUDUS_OS_WEB
    // One cooperative directory lock serializes Ludus writers. External edits
    // are checked again immediately before rename; arbitrary external writers
    // must participate in locking for a strict multi-process compare-and-swap.
    int lock;
    do
    {
        lock = ::flock(directory.Value, LOCK_EX | LOCK_NB);
    } while (lock < 0 && errno == EINTR);
    if (lock != 0)
    {
        return errno == EWOULDBLOCK || errno == EAGAIN ? Status::Conflict : Status::IoError;
    }
#    else
    // Thanks to Emscripten contributors, "File System API", MEMFS:
    // https://emscripten.org/docs/api_reference/Filesystem-API.html#memfs
    // This session filesystem has one owner thread. No yield occurs between
    // the checks and rename, so no flock is needed or supported. Publication
    // remains volatile; this does not implement browser persistence.
#    endif
    auto matches = [&]() noexcept {
        Bytes current;
        const Status readStatus = ReadAt(directory.Value, leaf.Data, MAX_SAVE_BYTES, current);
        if (expected == nullptr)
        {
            return readStatus == Status::NotFound ? Status::Ok
                   : readStatus == Status::Ok     ? Status::Conflict
                                                  : readStatus;
        }
        return readStatus == Status::NotFound      ? Status::Conflict
               : readStatus != Status::Ok          ? readStatus
               : Hash(current.Data()) == *expected ? Status::Ok
                                                   : Status::Conflict;
    };
    status = matches();
    if (status != Status::Ok)
    {
        return status;
    }
    Text<1100> temp;
    (void)temp.Set(leaf.View());
    // Exclusive creation prevents overwriting another operation's temporary.
    const auto suffix = std::string_view(".ludus-save");
    for (char ch : suffix)
    {
        temp.Data[temp.Length++] = ch;
    }
    temp.Data[temp.Length] = '\0';
    Fd file;
    file.Value = OpenAt(directory.Value, temp.Data, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file.Value < 0)
    {
        return Status::IoError;
    }
    usize offset = 0;
    status = Status::Ok;
    while (offset < data.size())
    {
        const auto count = native::Write(file.Value, data.subspan(offset));
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            status = Status::IoError;
            break;
        }
        offset += static_cast<usize>(count);
    }
    if (status == Status::Ok && Sync(file.Value) != 0)
    {
        status = Status::IoError;
    }
    if (file.Close() == false && status == Status::Ok)
    {
        status = Status::IoError;
    }
    if (status == Status::Ok)
    {
        status = matches();
    }
    if (status == Status::Ok && native::Replace(directory.Value, temp.Data, leaf.Data) != 0)
    {
        status = Status::IoError;
    }
    if (status != Status::Ok)
    {
        unlinkat(directory.Value, temp.Data, 0);
    }
    else
    {
        // Publication already succeeded. Directory sync is best-effort; the API
        // reports atomic publication, not power-loss durability.
        (void)Sync(directory.Value);
    }
    return status;
#else
    (void)root;
    (void)path;
    (void)data;
    (void)expected;
    return Status::Unsupported;
#endif
}
struct FileReader::Impl final
{
    foundation::filesystem::File File;
    uint64 Position = 0;
};
FileReader::~FileReader() noexcept
{
    delete mImpl;
}
Status FileReader::Open(std::string_view root, std::string_view path) noexcept
{
    if (root.empty() || ValidPath(path) == false)
    {
        return Status::Invalid;
    }
    foundation::filesystem::Directory directory;
    auto result = directory.Open(root);
    if (result.Succeeded() == false)
    {
        return ContentStatus(result.Code);
    }
    auto* next = new (std::nothrow) Impl();
    if (next == nullptr)
    {
        return Status::OutOfMemory;
    }
    result = directory.OpenRead(path, next->File);
    if (result.Succeeded() == false)
    {
        delete next;
        return ContentStatus(result.Code);
    }
    delete mImpl;
    mImpl = next;
    return Status::Ok;
}
Status FileReader::Clone(FileReader& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return Status::Invalid;
    }
    auto* next = new (std::nothrow) Impl();
    if (next == nullptr)
    {
        return Status::OutOfMemory;
    }
    const auto result = mImpl->File.Clone(next->File);
    if (result.Succeeded() == false)
    {
        delete next;
        return ContentStatus(result.Code);
    }
    delete output.mImpl;
    output.mImpl = next;
    return Status::Ok;
}
Status FileReader::Read(std::span<uint8> bytes, usize& count) noexcept
{
    count = 0;
    if (mImpl == nullptr)
    {
        return Status::Invalid;
    }
    const auto read = mImpl->File.ReadAt(mImpl->Position, bytes);
    if (read.Outcome.Succeeded() == false)
    {
        return ContentStatus(read.Outcome.Code);
    }
    count = read.BytesRead;
    mImpl->Position += count;
    return Status::Ok;
}
Status FileReader::Seek(int64 offset, bool relative) noexcept
{
    if (mImpl == nullptr)
    {
        return Status::Invalid;
    }
    const auto base = relative ? mImpl->Position : 0;
    if (offset < 0)
    {
        const auto magnitude = static_cast<uint64>(-(offset + 1)) + 1;
        if (magnitude > base)
        {
            return Status::Invalid;
        }
        mImpl->Position = base - magnitude;
    }
    else
    {
        const auto forward = static_cast<uint64>(offset);
        if (forward > Size() - base)
        {
            return Status::Invalid;
        }
        mImpl->Position = base + forward;
    }
    return Status::Ok;
}
uint64 FileReader::Size() const noexcept
{
    return mImpl != nullptr ? mImpl->File.Size() : 0;
}

} // namespace ludus::content
