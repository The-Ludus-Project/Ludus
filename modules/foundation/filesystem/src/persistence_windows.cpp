#include "internal/windows_files.hpp"

#if defined(LUDUS_FILESYSTEM_WINDOWS_RECOVERY_TESTING)
#    include "internal/windows_publication_test_hooks.hpp"
#endif

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/filesystem/persistence.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

namespace ludus::foundation::filesystem
{
namespace
{
using windows::NativeHandle;

constexpr std::wstring_view TEMPORARY_PREFIX = L".ludus-write-";
constexpr usize MAX_TEMPORARY_ATTEMPTS = 16;
constexpr usize HEX_DIGITS_PER_WORD = sizeof(uint64) * 2;
constexpr uint64 HEX_DIGIT_MASK = 0xF;
constexpr uint32 BITS_PER_HEX_DIGIT = 4;
constexpr usize TEMPORARY_NAME_LENGTH = TEMPORARY_PREFIX.size() + 2 * HEX_DIGITS_PER_WORD + 1;
constexpr wchar_t HEX_DIGITS[] = L"0123456789abcdef";
std::atomic<uint64> gSequence{1};

Result LockParent(HANDLE parent, NativeHandle& outLock) noexcept
{
    // Keep one stable lock file per directory. An exclusive write share is
    // nonblocking and automatically released on process exit. Never unlink it:
    // that could let peers lock different objects for the same directory.
    const Result result = windows::OpenChild(parent,
                                             windows::LOCK_NAME,
                                             FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ,
                                             FILE_OPEN_IF,
                                             FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                             outLock);
    if (result.NativeCode == ERROR_SHARING_VIOLATION)
    {
        return {Status::Conflict, result.NativeCode};
    }
    if (result.Succeeded() == false)
    {
        return result;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(outLock.Get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE)
    {
        return windows::MapError(GetLastError());
    }
    return (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? Result{Status::AccessDenied} : Result{};
}

Result CheckDestination(HANDLE parent, std::wstring_view name, const WriteOptions& options) noexcept
{
    NativeHandle file;
    const Result result = windows::OpenRegular(parent, name, 0, FILE_SYNCHRONOUS_IO_NONALERT, file);
    if (result.Code == Status::NotFound)
    {
        return options.Condition == WriteCondition::MatchingStamp ? Result{Status::Conflict} : Result{};
    }
    if (result.Succeeded() == false)
    {
        return result;
    }
    if (options.Condition == WriteCondition::Missing)
    {
        return {Status::Conflict};
    }
    if (options.Condition == WriteCondition::MatchingStamp)
    {
        FileStamp stamp;
        const Result observed = windows::ObserveFile(file.Get(), stamp);
        if (observed.Succeeded() == false)
        {
            return observed;
        }
        return stamp == options.Expected ? Result{} : Result{Status::Conflict};
    }
    return {};
}

void WriteHex(uint64 value, wchar_t* outDigits) noexcept
{
    for (usize digit = HEX_DIGITS_PER_WORD; digit > 0; --digit)
    {
        outDigits[digit - 1] = HEX_DIGITS[value & HEX_DIGIT_MASK];
        value >>= BITS_PER_HEX_DIGIT;
    }
}

Result CreateTemporary(HANDLE parent, std::wstring_view leaf, NativeHandle& outFile) noexcept
{
    StaticArray<wchar_t, TEMPORARY_NAME_LENGTH + 1> name{};
    for (usize i = 0; i < TEMPORARY_PREFIX.size(); ++i)
    {
        name[i] = TEMPORARY_PREFIX[i];
    }
    WriteHex(GetCurrentProcessId(), name.GetData() + TEMPORARY_PREFIX.size());
    constexpr usize SEPARATOR = TEMPORARY_PREFIX.size() + HEX_DIGITS_PER_WORD;
    name[SEPARATOR] = L'-';
    for (usize attempt = 0; attempt < MAX_TEMPORARY_ATTEMPTS; ++attempt)
    {
        const uint64 sequence = gSequence.fetch_add(1, std::memory_order_relaxed);
        WriteHex(sequence, name.GetData() + SEPARATOR + 1);
        if (CompareStringOrdinal(name.GetData(), -1, leaf.data(), static_cast<int32>(leaf.size()), TRUE) == CSTR_EQUAL)
        {
            continue;
        }
        const Result result = windows::OpenChild(parent,
                                                 {name.GetData(), TEMPORARY_NAME_LENGTH},
                                                 GENERIC_WRITE | DELETE,
                                                 FILE_SHARE_READ,
                                                 FILE_CREATE,
                                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                                 outFile);
        if (result.NativeCode != ERROR_FILE_EXISTS && result.NativeCode != ERROR_ALREADY_EXISTS)
        {
            return result;
        }
    }
    return {Status::LimitExceeded};
}

Result WriteBytes(HANDLE file, std::span<const uint8> bytes) noexcept
{
    usize total = 0;
    while (total < bytes.size())
    {
        const usize remaining = bytes.size() - total;
        const DWORD count =
            remaining < windows::MAX_TRANSFER_BYTES ? static_cast<DWORD>(remaining) : windows::MAX_TRANSFER_BYTES;
        DWORD written = 0;
        if (WriteFile(file, bytes.data() + total, count, &written, nullptr) == FALSE)
        {
            return windows::MapError(GetLastError());
        }
        if (written == 0)
        {
            return {Status::IoError};
        }
        total += written;
    }
    return {};
}

Result RenameTemporary(HANDLE file, HANDLE parent, std::wstring_view leaf, bool shouldReplace) noexcept
{
    // Thanks to Microsoft, "FILE_RENAME_INFO structure" and "FILE_RENAME_INFORMATION
    // structure", for the relative rename ABI and the POSIX replacement flag,
    // which lets delete-sharing readers continue using the previous revision.
    // https://learn.microsoft.com/windows/win32/api/winbase/ns-winbase-file_rename_info
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
    constexpr usize NAME_OFFSET = offsetof(FILE_RENAME_INFO, FileName);
    constexpr usize BUFFER_BYTES = sizeof(FILE_RENAME_INFO) + MAX_PATH_BYTES * sizeof(wchar_t);
    alignas(FILE_RENAME_INFO) StaticArray<uint8, BUFFER_BYTES> buffer{};
    FILE_RENAME_INFO* rename = new (buffer.GetData()) FILE_RENAME_INFO{};
    rename->Flags = shouldReplace == true ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS : 0;
    rename->RootDirectory = parent;
    rename->FileNameLength = static_cast<DWORD>(leaf.size() * sizeof(wchar_t));
    std::memcpy(buffer.GetData() + NAME_OFFSET, leaf.data(), rename->FileNameLength);
    const DWORD used = static_cast<DWORD>(NAME_OFFSET + rename->FileNameLength + sizeof(wchar_t));
    if (SetFileInformationByHandle(file, FileRenameInfoEx, rename, used) != FALSE)
    {
        return {};
    }
    const DWORD error = GetLastError();
    if (shouldReplace == false && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS))
    {
        return {Status::Conflict, static_cast<int32>(error)};
    }
    return windows::MapError(error);
}

Result SyncFile(HANDLE file) noexcept
{
    // Thanks to Microsoft, "FlushFileBuffers function", for the required access
    // and explicit result. A filesystem may refuse flushing a directory handle.
    // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
    return FlushFileBuffers(file) != FALSE ? Result{} : windows::MapError(GetLastError());
}

Result SyncParent(HANDLE parent) noexcept
{
    NativeHandle writable;
    writable.Reset(ReOpenFile(parent, GENERIC_WRITE, windows::SHARE_ALL, FILE_FLAG_BACKUP_SEMANTICS));
    if (writable.Get() == INVALID_HANDLE_VALUE)
    {
        return windows::MapError(GetLastError());
    }
    return SyncFile(writable.Get());
}

void FinishTemporary(NativeHandle& inoutTemporary, PublicationResult& inoutResult) noexcept
{
    if (inoutResult.Published == false)
    {
        FILE_DISPOSITION_INFO removal{TRUE};
        if (SetFileInformationByHandle(inoutTemporary.Get(), FileDispositionInfo, &removal, sizeof(removal)) == FALSE)
        {
            inoutResult.Cleanup = windows::MapError(GetLastError());
        }
    }
    const Result closed = inoutTemporary.Close();
    if (inoutResult.Cleanup.Succeeded() == true)
    {
        inoutResult.Cleanup = closed;
    }
}
} // namespace

struct WriteDirectory::Impl final
{
    NativeHandle Root;
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
    nextOrNull->Root.Reset(root.Release());
    (void)Close();
    mImpl = nextOrNull;
    return {};
}

Result WriteDirectory::Close() noexcept
{
    if (mImpl == nullptr)
    {
        return {};
    }
    const Result result = mImpl->Root.Close();
    delete mImpl;
    mImpl = nullptr;
    return result;
}

PublicationResult WriteDirectory::Publish(std::string_view relativePath,
                                          std::span<const uint8> bytes,
                                          const WriteOptions& options) const noexcept
{
    if (mImpl == nullptr ||
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

    const HANDLE root = mImpl->Root.Get();
    NativeHandle parent;
    windows::Component leafName;
    Result result = windows::ResolveParent(root, relativePath, parent, leafName);
    if (result.Succeeded() == false)
    {
        return {result};
    }
    const HANDLE directory = parent.Get() == INVALID_HANDLE_VALUE ? root : parent.Get();
    const std::wstring_view leaf(leafName.GetData());
    NativeHandle lock;
    result = LockParent(directory, lock);
    if (result.Succeeded() == false)
    {
        return {result};
    }
    result = CheckDestination(directory, leaf, options);
    if (result.Succeeded() == false)
    {
        return {result};
    }
    NativeHandle temporary;
    result = CreateTemporary(directory, leaf, temporary);
    if (result.Succeeded() == false)
    {
        return {result};
    }

    PublicationResult publication;
    publication.Outcome = WriteBytes(temporary.Get(), bytes);
    if (publication.Outcome.Succeeded() == true && options.Sync != SyncPolicy::PublishOnly)
    {
        publication.Outcome = SyncFile(temporary.Get());
        publication.FileSynced = publication.Outcome.Succeeded();
    }
    if (publication.Outcome.Succeeded() == true)
    {
        publication.Outcome = CheckDestination(directory, leaf, options);
    }
    if (publication.Outcome.Succeeded() == true)
    {
#if defined(LUDUS_FILESYSTEM_WINDOWS_RECOVERY_TESTING)
        detail::InterruptWindowsPublication(false);
#endif
        publication.Outcome =
            RenameTemporary(temporary.Get(), directory, leaf, options.Condition != WriteCondition::Missing);
        publication.Published = publication.Outcome.Succeeded();
    }
#if defined(LUDUS_FILESYSTEM_WINDOWS_RECOVERY_TESTING)
    if (publication.Published == true)
    {
        detail::InterruptWindowsPublication(true);
    }
#endif
    FinishTemporary(temporary, publication);
    if (publication.Published == true && options.Sync == SyncPolicy::FileAndDirectory)
    {
        publication.Outcome = SyncParent(directory);
        publication.DirectorySynced = publication.Outcome.Succeeded();
    }
    return publication;
}
} // namespace ludus::foundation::filesystem
