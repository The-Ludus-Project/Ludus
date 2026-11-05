#include "internal/windows_files.hpp"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <new>

namespace ludus::foundation::filesystem
{
namespace
{
using windows::NativeHandle;
constexpr uint32 NATIVE_OFFSET_WORD_BITS = 32;

Result CheckRevision(HANDLE file, const FileStamp& original) noexcept
{
    FileStamp current;
    const Result result = windows::ObserveFile(file, current);
    if (result.Succeeded() == false)
    {
        return result;
    }
    // Atomic replacement may change the old object's change time.
    return current.Size != original.Size || current.ModifiedSeconds != original.ModifiedSeconds ||
                   current.ModifiedNanoseconds != original.ModifiedNanoseconds
               ? Result{Status::Changed}
               : Result{};
}
} // namespace

struct File::Impl final
{
    NativeHandle Handle;
    FileStamp Original;
};

struct Directory::Impl final
{
    NativeHandle Handle;
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
    return mImpl != nullptr ? mImpl->Original.Size : 0;
}
Result File::Close() noexcept
{
    if (mImpl == nullptr)
    {
        return {};
    }
    const Result result = mImpl->Handle.Close();
    delete mImpl;
    mImpl = nullptr;
    return result;
}

Result File::Clone(File& outFile) const noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    HANDLE duplicateOrNull = nullptr;
    const HANDLE process = GetCurrentProcess();
    if (DuplicateHandle(process, mImpl->Handle.Get(), process, &duplicateOrNull, 0, FALSE, DUPLICATE_SAME_ACCESS) ==
        FALSE)
    {
        return windows::MapError(GetLastError());
    }
    NativeHandle duplicate;
    duplicate.Reset(duplicateOrNull);
    Impl* nextOrNull = new (std::nothrow) Impl;
    if (nextOrNull == nullptr)
    {
        return {Status::OutOfMemory};
    }
    nextOrNull->Original = mImpl->Original;
    nextOrNull->Handle.Reset(duplicate.Release());
    (void)outFile.Close();
    outFile.mImpl = nextOrNull;
    return {};
}

ReadResult File::ReadAt(uint64 offset, std::span<uint8> destination) const noexcept
{
    if (mImpl == nullptr || offset > Size())
    {
        return {{Status::InvalidArgument}};
    }
    const HANDLE file = mImpl->Handle.Get();
    const FileStamp& original = mImpl->Original;
    Result result = CheckRevision(file, original);
    if (result.Succeeded() == false)
    {
        return {result};
    }
    const uint64 available = original.Size - offset;
    const usize wanted = available < destination.size() ? static_cast<usize>(available) : destination.size();
    if (wanted == 0)
    {
        return {CheckRevision(file, original)};
    }

    // One completion event per call keeps simultaneous reads and clones independent.
    const HANDLE completionOrNull = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completionOrNull == nullptr)
    {
        return {windows::MapError(GetLastError())};
    }
    NativeHandle completion;
    completion.Reset(completionOrNull);
    usize total = 0;
    while (total < wanted)
    {
        const usize remaining = wanted - total;
        const DWORD count =
            remaining < windows::MAX_TRANSFER_BYTES ? static_cast<DWORD>(remaining) : windows::MAX_TRANSFER_BYTES;
        const uint64 position = offset + total;
        OVERLAPPED operation{};
        operation.Offset = static_cast<DWORD>(position);
        operation.OffsetHigh = static_cast<DWORD>(position >> NATIVE_OFFSET_WORD_BITS);
        operation.hEvent = completion.Get();
        DWORD transferred = 0;
        if (ReadFile(file, destination.data() + total, count, nullptr, &operation) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                result = windows::MapError(error);
                break;
            }
        }
        if (GetOverlappedResult(file, &operation, &transferred, TRUE) == FALSE)
        {
            result = windows::MapError(GetLastError());
            break;
        }
        if (transferred == 0)
        {
            result = {Status::IoError};
            break;
        }
        total += transferred;
    }
    const Result revision = CheckRevision(file, original);
    if (revision.Code == Status::Changed)
    {
        return {revision, 0};
    }
    return {result.Succeeded() == true ? revision : result, total};
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
    const Result result = mImpl->Handle.Close();
    delete mImpl;
    mImpl = nullptr;
    return result;
}

Result Directory::Open(std::string_view nativeRoot) noexcept
{
    NativeHandle root;
    const Result result = windows::OpenRoot(nativeRoot, root);
    if (result.Succeeded() == false)
    {
        return result;
    }
    Impl* nextOrNull = new (std::nothrow) Impl;
    if (nextOrNull == nullptr)
    {
        return {Status::OutOfMemory};
    }
    nextOrNull->Handle.Reset(root.Release());
    (void)Close();
    mImpl = nextOrNull;
    return {};
}

Result Directory::OpenRead(std::string_view relativePath, File& outFile) const noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    NativeHandle parent;
    windows::Component leaf;
    const HANDLE root = mImpl->Handle.Get();
    Result result = windows::ResolveParent(root, relativePath, parent, leaf);
    if (result.Succeeded() == false)
    {
        return result;
    }
    NativeHandle file;
    const HANDLE directory = parent.Get() == INVALID_HANDLE_VALUE ? root : parent.Get();
    result = windows::OpenRegular(directory, leaf.GetData(), FILE_READ_DATA, 0, file);
    if (result.Succeeded() == false)
    {
        return result;
    }
    FileStamp original;
    result = windows::ObserveFile(file.Get(), original);
    if (result.Succeeded() == false)
    {
        return result;
    }
    File::Impl* nextOrNull = new (std::nothrow) File::Impl;
    if (nextOrNull == nullptr)
    {
        return {Status::OutOfMemory};
    }
    nextOrNull->Original = original;
    nextOrNull->Handle.Reset(file.Release());
    (void)outFile.Close();
    outFile.mImpl = nextOrNull;
    return {};
}

Result Directory::Observe(std::string_view relativePath, FileStamp& outStamp) const noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    NativeHandle parent;
    windows::Component leaf;
    const HANDLE root = mImpl->Handle.Get();
    Result result = windows::ResolveParent(root, relativePath, parent, leaf);
    if (result.Succeeded() == false)
    {
        return result;
    }
    NativeHandle file;
    const HANDLE directory = parent.Get() == INVALID_HANDLE_VALUE ? root : parent.Get();
    result = windows::OpenRegular(directory, leaf.GetData(), 0, FILE_SYNCHRONOUS_IO_NONALERT, file);
    return result.Succeeded() == true ? windows::ObserveFile(file.Get(), outStamp) : result;
}
} // namespace ludus::foundation::filesystem
