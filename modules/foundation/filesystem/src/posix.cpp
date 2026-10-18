#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include "internal/posix_metadata.hpp"

// Thanks to the Linux man-pages project, open(2), pread(2) and close(2),
// https://man7.org/linux/man-pages/man2/open.2.html : descriptor
// revision lifetime, independent offsets, short reads and Linux close ownership
// inform this backend. The component walk adds ancestor symlink rejection; it
// does not claim openat2 sandbox guarantees. Independently implemented; see
// docs/architecture/filesystem.md, "Reference review and design revision".

// Thanks to Apple, XNU kern_descrip.c, fp_close_and_unlock (macOS 14):
// fdrelse precedes the fallible fg_drop/fo_close path, confirming Darwin's
// descriptor ownership rule. Independently implemented, no XNU code copied.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-10002.1.13/bsd/kern/kern_descrip.c

#if defined(LUDUS_FILESYSTEM_FAULT_TESTING)
#    include "internal/posix_test_hooks.hpp"
#endif

#include <new>
#include <span>
#include <string_view>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ludus::foundation::filesystem
{
namespace
{
#if defined(LUDUS_FILESYSTEM_FAULT_TESTING)
namespace native = test;
#else
namespace native
{
int OpenAt(int parent, const char* name, int flags) noexcept
{
    return ::openat(parent, name, flags);
}
int Duplicate(int descriptor) noexcept
{
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
}
int Metadata(int descriptor, struct stat* info) noexcept
{
    return ::fstat(descriptor, info);
}
isize Read(int descriptor, std::span<uint8> buffer, off_t offset) noexcept
{
    return ::pread(descriptor, buffer.data(), buffer.size(), offset);
}
int Close(int descriptor) noexcept
{
    return ::close(descriptor);
}
} // namespace native
#endif
template <typename T>
T* Allocate() noexcept
{
#if defined(LUDUS_FILESYSTEM_FAULT_TESTING)
    if (test::AllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T();
}
const timespec& ModificationTime(const struct stat& info) noexcept
{
#if defined(LUDUS_PLATFORM_MACOS)
    return info.st_mtimespec;
#else
    return info.st_mtim;
#endif
}
static_assert(sizeof(off_t) >= sizeof(int64));

Result NativeFailure(int error) noexcept
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
struct Descriptor final
{
    int Value = -1;
    Descriptor() noexcept = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() noexcept
    {
        (void)Close();
    }
    [[nodiscard]] Result Close() noexcept
    {
        const int value = Value;
        Value = -1;
        // Linux and Darwin release ownership before reporting close errors.
        // Never retry: the descriptor number may already have been reused.
        return value >= 0 && native::Close(value) != 0 ? NativeFailure(errno) : Result{};
    }
};
int OpenAt(int parent, const char* name, int flags) noexcept
{
    int value;
    do
    {
        value = native::OpenAt(parent, name, flags);
    } while (value < 0 && errno == EINTR);
    return value;
}
int Duplicate(int descriptor) noexcept
{
    int value;
    do
    {
        value = native::Duplicate(descriptor);
    } while (value < 0 && errno == EINTR);
    return value;
}
Result Metadata(int descriptor, struct stat& info) noexcept
{
    int value;
    do
    {
        value = native::Metadata(descriptor, &info);
    } while (value < 0 && errno == EINTR);
    return value == 0 ? Result{} : NativeFailure(errno);
}
Result CheckRevision(int descriptor, const struct stat& original) noexcept
{
    struct stat current{};
    const auto result = Metadata(descriptor, current);
    if (result.Succeeded() == false)
    {
        return result;
    }
    // ctime changes when a path is unlinked/replaced; that must not invalidate
    // an otherwise unchanged opened revision. Size/mtime detection is best-effort.
    if (current.st_size != original.st_size || ModificationTime(current).tv_sec != ModificationTime(original).tv_sec ||
        ModificationTime(current).tv_nsec != ModificationTime(original).tv_nsec)
    {
        return {Status::Changed};
    }
    return {};
}
void CopyName(std::string_view text, char* output) noexcept
{
    for (usize i = 0; i < text.size(); ++i)
    {
        output[i] = text[i];
    }
    output[text.size()] = '\0';
}
} // namespace

struct File::Impl final
{
    Descriptor Handle;
    struct stat Original{};
    uint64 Length = 0;
};
struct Directory::Impl final
{
    Descriptor Handle;
};
File::~File() noexcept
{
    (void)Close();
}
File::File(File&& other) noexcept : mImpl(other.mImpl)
{
    other.mImpl = nullptr;
}
File& File::operator=(File&& other) noexcept
{
    if (this != &other)
    {
        (void)Close();
        mImpl = other.mImpl;
        other.mImpl = nullptr;
    }
    return *this;
}
bool File::IsOpen() const noexcept
{
    return mImpl != nullptr;
}
uint64 File::Size() const noexcept
{
    return mImpl != nullptr ? mImpl->Length : 0;
}
Result File::Close() noexcept
{
    if (mImpl == nullptr)
    {
        return {};
    }
    const auto result = mImpl->Handle.Close();
    delete mImpl;
    mImpl = nullptr;
    return result;
}
Result File::Clone(File& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    auto* next = Allocate<Impl>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    next->Handle.Value = Duplicate(mImpl->Handle.Value);
    if (next->Handle.Value < 0)
    {
        const auto result = NativeFailure(errno);
        delete next;
        return result;
    }
    next->Original = mImpl->Original;
    next->Length = mImpl->Length;
    (void)output.Close();
    output.mImpl = next;
    return {};
}
ReadResult File::ReadAt(uint64 offset, std::span<uint8> destination) const noexcept
{
    if (mImpl == nullptr || offset > Size())
    {
        return {{Status::InvalidArgument}};
    }
    auto result = CheckRevision(mImpl->Handle.Value, mImpl->Original);
    if (result.Succeeded() == false)
    {
        return {result};
    }
    const uint64 left = Size() - offset;
    usize wanted = destination.size();
    if (left < wanted && TryIntegerCast(left, wanted) == false)
    {
        return {{Status::InvalidArgument}};
    }
    usize total = 0;
    while (total < wanted)
    {
        // Respect Linux's transfer cap; use the same conservative chunk on Darwin.
        constexpr usize MAX_TRANSFER = 0x7ffff000;
        const usize remaining = wanted - total;
        const usize count = remaining < MAX_TRANSFER ? remaining : MAX_TRANSFER;
        off_t nativeOffset = 0;
        // offset + total is bounded by captured Length (a nonnegative off_t).
        if (TryIntegerCast(offset + total, nativeOffset) == false)
        {
            return {{Status::InvalidArgument}, total};
        }
        isize received;
        do
        {
            received = native::Read(mImpl->Handle.Value, {destination.data() + total, count}, nativeOffset);
        } while (received < 0 && errno == EINTR);
        if (received < 0)
        {
            return {NativeFailure(errno), total};
        }
        if (received == 0)
        {
            result = CheckRevision(mImpl->Handle.Value, mImpl->Original);
            return {result.Succeeded() ? Result{Status::IoError} : result, result.Code == Status::Changed ? 0 : total};
        }
        total += static_cast<usize>(received);
    }
    result = CheckRevision(mImpl->Handle.Value, mImpl->Original);
    return {result, result.Code == Status::Changed ? 0 : total};
}
Directory::~Directory() noexcept
{
    (void)Close();
}
Directory::Directory(Directory&& other) noexcept : mImpl(other.mImpl)
{
    other.mImpl = nullptr;
}
Directory& Directory::operator=(Directory&& other) noexcept
{
    if (this != &other)
    {
        (void)Close();
        mImpl = other.mImpl;
        other.mImpl = nullptr;
    }
    return *this;
}
bool Directory::IsOpen() const noexcept
{
    return mImpl != nullptr;
}
Result Directory::Close() noexcept
{
    if (mImpl == nullptr)
    {
        return {};
    }
    const auto result = mImpl->Handle.Close();
    delete mImpl;
    mImpl = nullptr;
    return result;
}
Result Directory::Open(std::string_view nativeRoot) noexcept
{
    if (nativeRoot.empty() || nativeRoot.size() > MAX_ROOT_BYTES || nativeRoot.find('\0') != std::string_view::npos)
    {
        return {Status::InvalidArgument};
    }
    char name[MAX_ROOT_BYTES + 1];
    CopyName(nativeRoot, name);
    auto* next = Allocate<Impl>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    next->Handle.Value = OpenAt(AT_FDCWD, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (next->Handle.Value < 0)
    {
        const auto result = NativeFailure(errno);
        delete next;
        return result;
    }
    (void)Close();
    mImpl = next;
    return {};
}
Result Directory::OpenRead(std::string_view relativePath, File& output) const noexcept
{
    if (mImpl == nullptr || ValidPath(relativePath) == false)
    {
        return {Status::InvalidArgument};
    }
    Descriptor parent;
    int current = mImpl->Handle.Value;
    usize begin = 0;
    char name[MAX_PATH_BYTES + 1];
    while (true)
    {
        const auto slash = relativePath.find('/', begin);
        const auto end = slash == std::string_view::npos ? relativePath.size() : slash;
        CopyName(relativePath.substr(begin, end - begin), name);
        if (slash == std::string_view::npos)
        {
            break;
        }
        const int next = OpenAt(current, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            return NativeFailure(errno);
        }
        (void)parent.Close();
        parent.Value = next;
        current = next;
        begin = slash + 1;
    }
    Descriptor file;
    // NONBLOCK avoids blocking on a FIFO before fstat rejects non-regular files.
    file.Value = OpenAt(current, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (file.Value < 0)
    {
        return NativeFailure(errno);
    }
    struct stat info{};
    const auto result = Metadata(file.Value, info);
    if (result.Succeeded() == false)
    {
        return result;
    }
    if (S_ISREG(info.st_mode) == false || info.st_size < 0)
    {
        return {Status::NotRegularFile};
    }
    auto* next = Allocate<File::Impl>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    if (TryIntegerCast(info.st_size, next->Length) == false)
    {
        delete next;
        return {Status::InvalidArgument};
    }
    next->Original = info;
    next->Handle.Value = file.Value;
    file.Value = -1;
    (void)output.Close();
    output.mImpl = next;
    return {};
}
Result Directory::Observe(std::string_view relativePath, FileStamp& output) const noexcept
{
    if (mImpl == nullptr || ValidPath(relativePath) == false)
    {
        return {Status::InvalidArgument};
    }
    Descriptor parent;
    int current = mImpl->Handle.Value;
    usize begin = 0;
    char name[MAX_PATH_BYTES + 1];
    while (true)
    {
        const auto slash = relativePath.find('/', begin);
        const auto end = slash == std::string_view::npos ? relativePath.size() : slash;
        CopyName(relativePath.substr(begin, end - begin), name);
        if (slash == std::string_view::npos)
        {
            break;
        }
        const int next = OpenAt(current, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            return NativeFailure(errno);
        }
        (void)parent.Close();
        parent.Value = next;
        current = next;
        begin = slash + 1;
    }
    struct stat info{};
    int value;
    do
    {
        value = ::fstatat(current, name, &info, AT_SYMLINK_NOFOLLOW);
    } while (value < 0 && errno == EINTR);
    if (value < 0)
    {
        return NativeFailure(errno);
    }
    if (S_ISLNK(info.st_mode))
    {
        return {Status::AccessDenied};
    }
    if (S_ISREG(info.st_mode) == false || info.st_size < 0)
    {
        return {Status::NotRegularFile};
    }
    output = detail::Stamp(info);
    return {};
}
} // namespace ludus::foundation::filesystem
