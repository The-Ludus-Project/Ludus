#include "native_files.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/strings/static_string.hpp>

#include <atomic>

// The browser build uses Emscripten's POSIX layer over its in-memory filesystem (MEMFS).
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_WEB
#    include <cerrno>
#    include <climits>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
#    include <dirent.h>
#    include <fcntl.h>
#    include <sys/file.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    define LUDUS_SCENE_COOK_POSIX 1
#else
#    define LUDUS_SCENE_COOK_POSIX 0
#endif

namespace ludus::content::scene_cook::detail
{
#if LUDUS_SCENE_COOK_POSIX
namespace
{
constexpr usize MAX_NATIVE_PATH_BYTES = 4096;
/// Bounded, always NUL-terminated native path; overflow is reported, never truncated.
using NativePath = foundation::StaticString<MAX_NATIVE_PATH_BYTES>;
/// Attempts at a fresh temporary name before reporting an I/O error.
constexpr uint32 MAX_TEMPORARY_ATTEMPTS = 64;

std::atomic<uint32> gTemporaryCounter{0};

/// Copy path into a NativePath; rejects empty paths and embedded NUL bytes.
[[nodiscard]] bool TryMakeNativePath(std::string_view path, NativePath& outPath) noexcept
{
    if (path.empty() == true || path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    return outPath.TryAssign(path) == foundation::StringStatus::Ok;
}

/// stat(2) the path and report whether it names a directory.
[[nodiscard]] bool IsDirectory(const char* path) noexcept
{
    struct stat fileStatus = {};
    return stat(path, &fileStatus) == 0 && S_ISDIR(fileStatus.st_mode);
}

/// Append the decimal digits of value.
[[nodiscard]] bool TryAppendDecimal(NativePath& inoutPath, uint64 value) noexcept
{
    char digits[20]{};
    usize count = 0;
    do
    {
        digits[sizeof(digits) - 1 - count] = static_cast<char>('0' + value % 10);
        value /= 10;
        ++count;
    } while (value != 0);
    return inoutPath.TryAppend(std::string_view(digits + sizeof(digits) - count, count)) ==
           foundation::StringStatus::Ok;
}
} // namespace

Status ResolveDirectory(std::string_view path, foundation::String& outPath) noexcept
{
    NativePath input;
    if (TryMakeNativePath(path, input) == false)
    {
        return Status::Invalid;
    }
    // realpath(3) writes into a caller buffer of at least PATH_MAX bytes; the
    // result is copied into the engine String straight away.
    char resolved[PATH_MAX + 1]{};
    if (realpath(input.GetData(), resolved) == nullptr)
    {
        return errno == ENOENT || errno == ENOTDIR ? Status::NotFound : Status::IoError;
    }
    if (IsDirectory(resolved) == false)
    {
        return Status::NotFound;
    }
    return outPath.TryAssign(resolved) == foundation::StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

Status MakeDirectories(std::string_view path) noexcept
{
    NativePath full;
    if (TryMakeNativePath(path, full) == false)
    {
        return Status::Invalid;
    }
    NativePath prefix;
    for (usize i = 1; i <= path.size(); ++i)
    {
        if (i != path.size() && path[i] != '/')
        {
            continue;
        }
        if (prefix.TryAssign(path.substr(0, i)) != foundation::StringStatus::Ok)
        {
            return Status::Invalid;
        }
        if (mkdir(prefix.GetData(), 0777) != 0 && errno != EEXIST)
        {
            return Status::IoError;
        }
    }
    return IsDirectory(full.GetData()) == true ? Status::Ok : Status::IoError;
}

Status WriteFileAtomic(std::string_view directory, std::string_view name, std::span<const uint8> data) noexcept
{
    NativePath target;
    if (target.TryAssign(directory) != foundation::StringStatus::Ok ||
        target.TryAppend("/") != foundation::StringStatus::Ok || target.TryAppend(name) != foundation::StringStatus::Ok)
    {
        return Status::Limit;
    }
    // Create the temporary with mode 0666 and O_EXCL so the kernel applies the
    // process umask itself, as the reference converter's open() did. The umask
    // is never read or changed, so concurrent threads are unaffected.
    NativePath temporary;
    int file = -1;
    for (uint32 attempt = 0; attempt < MAX_TEMPORARY_ATTEMPTS && file < 0; ++attempt)
    {
        temporary.Clear();
        const bool isNamed =
            temporary.TryAssign(target.GetView()) == foundation::StringStatus::Ok &&
            temporary.TryAppend(".ludus-cook-") == foundation::StringStatus::Ok &&
            TryAppendDecimal(temporary, static_cast<uint64>(getpid())) == true &&
            temporary.TryAppend("-") == foundation::StringStatus::Ok &&
            TryAppendDecimal(temporary, gTemporaryCounter.fetch_add(1, std::memory_order_relaxed)) == true;
        if (isNamed == false)
        {
            return Status::Limit;
        }
        file = open(temporary.GetData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (file < 0 && errno != EEXIST && errno != EINTR)
        {
            return Status::IoError;
        }
    }
    if (file < 0)
    {
        return Status::IoError;
    }
    usize written = 0;
    bool isOk = true;
    while (isOk == true && written < data.size())
    {
        const ssize_t count = write(file, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        isOk = count > 0;
        written += isOk == true ? static_cast<usize>(count) : 0;
    }
    isOk = isOk == true && fsync(file) == 0;
    isOk = close(file) == 0 && isOk == true;
    if (isOk == false || rename(temporary.GetData(), target.GetData()) != 0)
    {
        (void)unlink(temporary.GetData());
        return Status::IoError;
    }
    return Status::Ok;
}

namespace
{
/// Resolution state for ResolvePath (posixpath._joinrealpath, strict=False).
constexpr usize MAX_SYMLINK_FOLLOWS = 40;

[[nodiscard]] bool TryAppendComponent(NativePath& inoutPath, std::string_view name) noexcept
{
    if (inoutPath.GetView() != "/" && inoutPath.TryAppend("/") != foundation::StringStatus::Ok)
    {
        return false;
    }
    return inoutPath.TryAppend(name) == foundation::StringStatus::Ok;
}

void RemoveLastComponent(NativePath& inoutPath) noexcept
{
    const std::string_view view = inoutPath.GetView();
    const usize slash = view.rfind('/');
    NativePath parent;
    (void)parent.TryAssign(slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                                         : view.substr(0, slash));
    inoutPath = parent;
}

/// Join rest onto the absolute, already resolved inoutPath.
[[nodiscard]] Status JoinResolved(NativePath& inoutPath, std::string_view rest, usize& inoutFollows) noexcept
{
    if (rest.empty() == false && rest.front() == '/')
    {
        (void)inoutPath.TryAssign("/");
    }
    while (rest.empty() == false)
    {
        const usize slash = rest.find('/');
        const std::string_view name = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (name.empty() == true || name == ".")
        {
            continue;
        }
        if (name == "..")
        {
            RemoveLastComponent(inoutPath);
            continue;
        }
        NativePath candidate = inoutPath;
        if (TryAppendComponent(candidate, name) == false)
        {
            return Status::Limit;
        }
        struct stat fileStatus = {};
        if (lstat(candidate.GetData(), &fileStatus) != 0 || S_ISLNK(fileStatus.st_mode) == false)
        {
            inoutPath = candidate;
            continue;
        }
        if (++inoutFollows > MAX_SYMLINK_FOLLOWS)
        {
            // A loop: Python keeps the unresolved remainder.
            inoutPath = candidate;
            for (; rest.empty() == false;)
            {
                const usize next = rest.find('/');
                const std::string_view part = rest.substr(0, next);
                rest = next == std::string_view::npos ? std::string_view() : rest.substr(next + 1);
                if (part.empty() == false && TryAppendComponent(inoutPath, part) == false)
                {
                    return Status::Limit;
                }
            }
            return Status::Ok;
        }
        char target[MAX_NATIVE_PATH_BYTES + 1]{};
        const ssize_t count = readlink(candidate.GetData(), target, MAX_NATIVE_PATH_BYTES);
        if (count < 0)
        {
            inoutPath = candidate;
            continue;
        }
        const Status status =
            JoinResolved(inoutPath, std::string_view(target, static_cast<usize>(count)), inoutFollows);
        if (status != Status::Ok)
        {
            return status;
        }
    }
    return Status::Ok;
}

[[nodiscard]] Status WriteAll(int file, std::span<const uint8> data) noexcept
{
    usize written = 0;
    while (written < data.size())
    {
        const ssize_t count = write(file, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return Status::IoError;
        }
        written += static_cast<usize>(count);
    }
    return Status::Ok;
}

[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

[[nodiscard]] Status RemoveTreeAt(int parent, const char* name) noexcept
{
    struct stat fileStatus = {};
    if (fstatat(parent, name, &fileStatus, AT_SYMLINK_NOFOLLOW) != 0)
    {
        return errno == ENOENT ? Status::Ok : Status::IoError;
    }
    if (S_ISDIR(fileStatus.st_mode) == false)
    {
        return unlinkat(parent, name, 0) == 0 ? Status::Ok : Status::IoError;
    }
    const int directory = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
    {
        return Status::IoError;
    }
    DIR* stream = fdopendir(directory);
    if (stream == nullptr)
    {
        (void)close(directory);
        return Status::IoError;
    }
    Status status = Status::Ok;
    while (const dirent* entry = readdir(stream))
    {
        const std::string_view entryName(entry->d_name);
        if (entryName == "." || entryName == "..")
        {
            continue;
        }
        if (RemoveTreeAt(directory, entry->d_name) != Status::Ok)
        {
            status = Status::IoError;
        }
    }
    (void)closedir(stream);
    if (status == Status::Ok && unlinkat(parent, name, AT_REMOVEDIR) != 0)
    {
        status = Status::IoError;
    }
    return status;
}
} // namespace

Status ResolvePath(std::string_view path, foundation::String& outPath) noexcept
{
    if (path.empty() == true || path.find('\0') != std::string_view::npos || path.size() > MAX_NATIVE_PATH_BYTES)
    {
        return Status::Invalid;
    }
    NativePath resolved;
    if (path.front() != '/')
    {
        char current[MAX_NATIVE_PATH_BYTES + 1]{};
        if (getcwd(current, sizeof(current)) == nullptr)
        {
            return Status::IoError;
        }
        (void)resolved.TryAssign(current);
    }
    else
    {
        (void)resolved.TryAssign("/");
    }
    usize follows = 0;
    const Status status = JoinResolved(resolved, path, follows);
    if (status != Status::Ok)
    {
        return status;
    }
    return outPath.TryAssign(resolved.GetView()) == foundation::StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

bool DoesPathExist(std::string_view path) noexcept
{
    NativePath native;
    struct stat fileStatus = {};
    return TryMakeNativePath(path, native) == true && stat(native.GetData(), &fileStatus) == 0;
}

Status ReadFileBounded(std::string_view path, usize maximum, foundation::Array<uint8>& outData) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    struct stat fileStatus = {};
    if (stat(native.GetData(), &fileStatus) != 0)
    {
        return errno == ENOENT ? Status::NotFound : Status::IoError;
    }
    if (static_cast<uint64>(fileStatus.st_size) > maximum)
    {
        return Status::Limit;
    }
    const int file = open(native.GetData(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
    {
        return Status::IoError;
    }
    outData.Clear();
    Status status = Status::Ok;
    uint8 buffer[65536];
    while (true)
    {
        const ssize_t count = read(file, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0)
        {
            status = Status::IoError;
            break;
        }
        if (count == 0)
        {
            break;
        }
        if (outData.GetSize() + static_cast<usize>(count) > maximum)
        {
            status = Status::Limit;
            break;
        }
        if (outData.TryAddRange(std::span<const uint8>(buffer, static_cast<usize>(count))) == false)
        {
            status = Status::OutOfMemory;
            break;
        }
    }
    (void)close(file);
    return status;
}

Status HashFile(std::string_view path, content::Digest& outDigest) noexcept
{
    SequentialFile file;
    const Status opened = file.Open(path);
    if (opened != Status::Ok)
    {
        return opened;
    }
    content::Hasher hasher;
    uint8 buffer[65536];
    while (true)
    {
        usize count = 0;
        const Status read = file.Read(std::span<uint8>(buffer, sizeof(buffer)), count);
        if (read != Status::Ok)
        {
            return read;
        }
        if (count == 0)
        {
            break;
        }
        if (hasher.Add(std::span<const uint8>(buffer, count)) == false)
        {
            return Status::Limit;
        }
    }
    outDigest = hasher.Finish();
    return Status::Ok;
}

void SequentialFile::Close() noexcept
{
    if (mDescriptor >= 0)
    {
        (void)close(mDescriptor);
        mDescriptor = -1;
    }
}

Status SequentialFile::Open(std::string_view path) noexcept
{
    NativePath native;
    if (mDescriptor >= 0 || TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    mDescriptor = open(native.GetData(), O_RDONLY | O_CLOEXEC);
    if (mDescriptor < 0)
    {
        return errno == ENOENT ? Status::NotFound : Status::IoError;
    }
    return Status::Ok;
}

Status SequentialFile::Read(std::span<uint8> outBuffer, usize& outCount) noexcept
{
    outCount = 0;
    if (mDescriptor < 0)
    {
        return Status::IoError;
    }
    while (true)
    {
        const ssize_t count = read(mDescriptor, outBuffer.data(), outBuffer.size());
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0)
        {
            return Status::IoError;
        }
        outCount = static_cast<usize>(count);
        return Status::Ok;
    }
}

namespace
{
/// Publish without replacing an immutable artifact. MEMFS has no hard links; the
/// single-threaded browser cooker performs no yield between this check and rename.
/// This is session-memory publication, with no cross-process or disk durability claim.
int PublishImmutableTemporary(const char* temporary, const char* target) noexcept
{
#    if LUDUS_TARGET_OS == LUDUS_OS_WEB
    struct stat fileStatus = {};
    if (stat(target, &fileStatus) == 0)
    {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT)
    {
        return -1;
    }
    return rename(temporary, target);
#    else
    return link(temporary, target);
#    endif
}
} // namespace

PublishResult PublishFile(std::string_view path,
                          std::span<const uint8> data,
                          PublishMode mode,
                          const Cancellation* cancelOrNull) noexcept
{
    NativePath target;
    if (TryMakeNativePath(path, target) == false)
    {
        return PublishResult::IoError;
    }
    const usize slash = path.rfind('/');
    const std::string_view parent = slash == std::string_view::npos ? std::string_view(".")
                                    : slash == 0                    ? std::string_view("/")
                                                                    : path.substr(0, slash);
    if (MakeDirectories(parent) != Status::Ok)
    {
        return PublishResult::IoError;
    }
    NativePath temporary;
    if (temporary.TryAssign(parent) != foundation::StringStatus::Ok ||
        temporary.TryAppend("/.pending-XXXXXX") != foundation::StringStatus::Ok)
    {
        return PublishResult::IoError;
    }
    char pending[MAX_NATIVE_PATH_BYTES + 1]{};
    std::memcpy(pending, temporary.GetData(), temporary.GetSize());
    const int file = mkstemp(pending);
    if (file < 0)
    {
        return PublishResult::IoError;
    }
    (void)temporary.TryAssign(pending);
    PublishResult result = PublishResult::Ok;
    bool isWritten = WriteAll(file, data) == Status::Ok && fsync(file) == 0;
    isWritten = close(file) == 0 && isWritten == true;
    // NOLINTBEGIN(bugprone-branch-clone): write and rename failures both map to IoError.
    if (isWritten == false)
    {
        result = PublishResult::IoError;
    }
    else if (IsCancelled(cancelOrNull) == true)
    {
        result = PublishResult::Cancelled;
    }
    else if (mode == PublishMode::Immutable)
    {
        if (PublishImmutableTemporary(temporary.GetData(), target.GetData()) != 0)
        {
            if (errno != EEXIST)
            {
                result = PublishResult::IoError;
            }
            else
            {
                foundation::Array<uint8> existing;
                if (ReadFileBounded(path, MAX_SOURCE_BYTES, existing) != Status::Ok)
                {
                    result = PublishResult::ReadFailure;
                }
                else if (existing.GetSize() != data.size() ||
                         (data.empty() == false && std::memcmp(existing.GetData(), data.data(), data.size()) != 0))
                {
                    result = PublishResult::Conflict;
                }
            }
        }
    }
    else if (rename(temporary.GetData(), target.GetData()) != 0)
    {
        result = PublishResult::IoError;
    }
    // NOLINTEND(bugprone-branch-clone)
    if (result == PublishResult::Ok && SyncDirectory(parent) != Status::Ok)
    {
        result = PublishResult::IoError;
    }
    (void)unlink(temporary.GetData());
    return result;
}

Status RenamePath(std::string_view from, std::string_view to) noexcept
{
    NativePath source;
    NativePath destination;
    if (TryMakeNativePath(from, source) == false || TryMakeNativePath(to, destination) == false)
    {
        return Status::Invalid;
    }
    return rename(source.GetData(), destination.GetData()) == 0 ? Status::Ok : Status::IoError;
}

Status RemoveFile(std::string_view path) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    return unlink(native.GetData()) == 0 ? Status::Ok : Status::IoError;
}

Status MakeDirectory(std::string_view path) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    return mkdir(native.GetData(), 0777) == 0 ? Status::Ok : Status::IoError;
}

Status SyncDirectory(std::string_view path) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    const int directory = open(native.GetData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
    {
        return Status::IoError;
    }
    const bool isSynced = fsync(directory) == 0;
    (void)close(directory);
    return isSynced == true ? Status::Ok : Status::IoError;
}

Status MakeTemporaryDirectory(std::string_view parent, std::string_view prefix, foundation::String& outPath) noexcept
{
    NativePath native;
    if (native.TryAssign(parent) != foundation::StringStatus::Ok ||
        native.TryAppend("/") != foundation::StringStatus::Ok ||
        native.TryAppend(prefix) != foundation::StringStatus::Ok ||
        native.TryAppend("XXXXXX") != foundation::StringStatus::Ok)
    {
        return Status::Limit;
    }
    char created[MAX_NATIVE_PATH_BYTES + 1]{};
    std::memcpy(created, native.GetData(), native.GetSize());
    if (mkdtemp(created) == nullptr)
    {
        return Status::IoError;
    }
    return outPath.TryAssign(created) == foundation::StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

Status RemoveTree(std::string_view path) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    return RemoveTreeAt(AT_FDCWD, native.GetData());
}

FileLock::~FileLock() noexcept
{
    if (mDescriptor >= 0)
    {
#    if LUDUS_TARGET_OS != LUDUS_OS_WEB
        (void)flock(mDescriptor, LOCK_UN);
#    endif
        (void)close(mDescriptor);
    }
}

Status FileLock::Acquire(std::string_view path, LockMode mode) noexcept
{
    NativePath native;
    if (mDescriptor >= 0 || TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    const int flags =
        mode == LockMode::Exclusive ? (O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
    const int file = open(native.GetData(), flags, 0666);
    if (file < 0)
    {
        return Status::IoError;
    }
#    if LUDUS_TARGET_OS != LUDUS_OS_WEB
    if (flock(file, (mode == LockMode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
    {
        const bool isBusy = errno == EWOULDBLOCK;
        (void)close(file);
        return isBusy == true ? Status::Conflict : Status::IoError;
    }
#    endif
    // The browser editor is one single-threaded process and MEMFS has no other writers,
    // so it keeps the descriptor without an advisory lock.
    mDescriptor = file;
    return Status::Ok;
}
#else
Status ResolveDirectory(std::string_view, foundation::String&) noexcept
{
    return Status::Unsupported;
}
Status MakeDirectories(std::string_view) noexcept
{
    return Status::Unsupported;
}
Status WriteFileAtomic(std::string_view, std::string_view, std::span<const uint8>) noexcept
{
    return Status::Unsupported;
}
Status ResolvePath(std::string_view, foundation::String&) noexcept
{
    return Status::Unsupported;
}
bool DoesPathExist(std::string_view) noexcept
{
    return false;
}
Status ReadFileBounded(std::string_view, usize, foundation::Array<uint8>&) noexcept
{
    return Status::Unsupported;
}
Status HashFile(std::string_view, content::Digest&) noexcept
{
    return Status::Unsupported;
}
PublishResult PublishFile(std::string_view, std::span<const uint8>, PublishMode, const Cancellation*) noexcept
{
    return PublishResult::IoError;
}
Status RenamePath(std::string_view, std::string_view) noexcept
{
    return Status::Unsupported;
}
Status RemoveFile(std::string_view) noexcept
{
    return Status::Unsupported;
}
Status MakeDirectory(std::string_view) noexcept
{
    return Status::Unsupported;
}
Status SyncDirectory(std::string_view) noexcept
{
    return Status::Unsupported;
}
Status MakeTemporaryDirectory(std::string_view, std::string_view, foundation::String&) noexcept
{
    return Status::Unsupported;
}
Status RemoveTree(std::string_view) noexcept
{
    return Status::Unsupported;
}
void SequentialFile::Close() noexcept {}
Status SequentialFile::Open(std::string_view) noexcept
{
    return Status::Unsupported;
}
Status SequentialFile::Read(std::span<uint8>, usize& outCount) noexcept
{
    outCount = 0;
    return Status::Unsupported;
}
Status FileLock::Acquire(std::string_view, LockMode) noexcept
{
    return Status::Unsupported;
}
#endif

std::string_view GetBaseName(std::string_view path) noexcept
{
    while (path.size() > 1 && path.back() == '/')
    {
        path.remove_suffix(1);
    }
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}
} // namespace ludus::content::scene_cook::detail
