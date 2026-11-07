#include "native_files.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <atomic>
#include <cstring>

#include <windows.h>

// Win32 implementation of the scene pipeline's native file helpers (D6a). It
// keeps the POSIX helpers' contracts, mapping each Python operation the
// reference converter performed onto its Windows equivalent:
// os.path.realpath (ntpath, strict=False) onto GetFullPathNameW and
// GetFinalPathNameByHandleW, os.replace onto MoveFileExW, os.link onto
// CreateHardLinkW and fsync onto FlushFileBuffers. Paths cross the boundary as
// UTF-8 with '/' separators and are converted to extended-length UTF-16 paths.
// Thanks to Microsoft, "Maximum Path Length Limitation" and "Naming Files,
// Paths, and Namespaces", for the \\?\ and \\?\UNC\ prefixes and the
// separator rules: https://learn.microsoft.com/windows/win32/fileio/naming-a-file
// Thanks to the Python Software Foundation, CPython 3.12 Lib/ntpath.py,
// realpath and _getfinalpathname_nonstrict, for the nonstrict resolution of a
// missing tail that ResolvePath reproduces.
namespace ludus::content::scene_cook::detail
{
namespace
{
/// Bounded, always NUL-terminated UTF-8 path with '/' separators.
using NativePath = foundation::StaticString<MAX_NATIVE_PATH_BYTES>;
/// UTF-16 units for any admitted path plus the extended-length prefix and terminator.
constexpr usize MAX_WIDE_PATH_UNITS = MAX_NATIVE_PATH_BYTES + 16;
using WidePath = foundation::StaticArray<wchar_t, MAX_WIDE_PATH_UNITS>;
/// Bytes per read or write call; well below the 32-bit DWORD transfer limit.
constexpr DWORD MAX_TRANSFER_BYTES = static_cast<DWORD>(NATIVE_READ_CHUNK_BYTES);
constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
/// Nested directories RemoveTree descends before reporting an error.
constexpr uint32 MAX_REMOVE_DEPTH = 256;
/// LockFileEx range: one byte at the largest offset, so locks never cover file data.
constexpr DWORD LOCK_OFFSET_LOW = 0xFFFF'FFFEU;
constexpr DWORD LOCK_OFFSET_HIGH = 0x7FFF'FFFFU;
constexpr std::wstring_view EXTENDED_PREFIX = L"\\\\?\\";
constexpr std::wstring_view EXTENDED_UNC_PREFIX = L"\\\\?\\UNC\\";
constexpr std::wstring_view DEVICE_PREFIX = L"\\\\.\\";
/// Units FindFirstFileExW needs after a directory name: "\\*" and the terminator.
constexpr usize SEARCH_SUFFIX_UNITS = 3;
constexpr usize UNC_LEADING_SEPARATORS = 2;

std::atomic<uint32> gTemporaryCounter{0};

/// Owns a Win32 HANDLE; INVALID_HANDLE_VALUE when empty.
class ScopedHandle final
{
public:
    explicit ScopedHandle(HANDLE handle) noexcept : mHandle(handle) {}
    ~ScopedHandle() noexcept
    {
        (void)Close();
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return mHandle;
    }
    [[nodiscard]] bool IsValid() const noexcept
    {
        return mHandle != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE Release() noexcept
    {
        const HANDLE handle = mHandle;
        mHandle = INVALID_HANDLE_VALUE;
        return handle;
    }
    /// Close now; false when CloseHandle reports an error.
    [[nodiscard]] bool Close() noexcept
    {
        const HANDLE handle = Release();
        return handle == INVALID_HANDLE_VALUE || CloseHandle(handle) != FALSE;
    }

private:
    HANDLE mHandle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool IsMissingError(DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

/// Copy a UTF-8 path, turning '\\' into '/'; rejects empty paths, NUL and overflow.
[[nodiscard]] bool TryMakeNativePath(std::string_view path, NativePath& outPath) noexcept
{
    outPath.Clear();
    if (path.empty() == true || path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    usize begin = 0;
    for (usize i = 0; i <= path.size(); ++i)
    {
        if (i != path.size() && path[i] != '\\')
        {
            continue;
        }
        if (outPath.TryAppend(path.substr(begin, i - begin)) != foundation::StringStatus::Ok ||
            (i != path.size() && outPath.TryAppend("/") != foundation::StringStatus::Ok))
        {
            return false;
        }
        begin = i + 1;
    }
    return true;
}

/// UTF-8 to NUL-terminated UTF-16 in outPath starting at offset; returns the unit count or zero.
[[nodiscard]] usize DecodeUtf8(std::string_view text, WidePath& outPath, usize offset) noexcept
{
    if (text.empty() == true || offset >= outPath.GetSize())
    {
        return 0;
    }
    const int32 units = MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            text.data(),
                                            static_cast<int32>(text.size()),
                                            outPath.GetData() + offset,
                                            static_cast<int32>(outPath.GetSize() - offset - 1));
    if (units <= 0)
    {
        return 0;
    }
    outPath[offset + static_cast<usize>(units)] = L'\0';
    return static_cast<usize>(units);
}

/// UTF-16 to UTF-8 with '/' separators, replacing outPath.
[[nodiscard]] bool EncodeUtf8(std::wstring_view text, NativePath& outPath) noexcept
{
    outPath.Clear();
    if (text.empty() == true)
    {
        return false;
    }
    foundation::StaticArray<char, MAX_NATIVE_PATH_BYTES + 1> bytes;
    const int32 count = WideCharToMultiByte(CP_UTF8,
                                            WC_ERR_INVALID_CHARS,
                                            text.data(),
                                            static_cast<int32>(text.size()),
                                            bytes.GetData(),
                                            static_cast<int32>(MAX_NATIVE_PATH_BYTES),
                                            nullptr,
                                            nullptr);
    if (count <= 0)
    {
        return false;
    }
    return TryMakeNativePath(std::string_view(bytes.GetData(), static_cast<usize>(count)), outPath);
}

/// Absolute, lexically normalized UTF-16 path (GetFullPathNameW: '/' becomes '\\',
/// "." and ".." collapse, relative and drive-relative paths join the current directory).
[[nodiscard]] bool GetFullPath(std::string_view path, WidePath& outPath) noexcept
{
    WidePath input;
    if (path.empty() == true || path.find('\0') != std::string_view::npos || DecodeUtf8(path, input, 0) == 0)
    {
        return false;
    }
    const DWORD units =
        GetFullPathNameW(input.GetData(), static_cast<DWORD>(outPath.GetSize()), outPath.GetData(), nullptr);
    return units != 0 && units < outPath.GetSize();
}

/// Extended-length form of an absolute path from GetFullPath, so MAX_PATH does not apply.
[[nodiscard]] bool MakeExtendedPath(const wchar_t* fullPath, WidePath& outPath) noexcept
{
    const std::wstring_view full(fullPath);
    std::wstring_view prefix = EXTENDED_PREFIX;
    std::wstring_view rest = full;
    if (full.substr(0, EXTENDED_PREFIX.size()) == EXTENDED_PREFIX ||
        full.substr(0, DEVICE_PREFIX.size()) == DEVICE_PREFIX)
    {
        prefix = {};
    }
    else if (full.substr(0, UNC_LEADING_SEPARATORS) == L"\\\\")
    {
        prefix = EXTENDED_UNC_PREFIX;
        rest = full.substr(UNC_LEADING_SEPARATORS);
    }
    if (prefix.size() + rest.size() + 1 > outPath.GetSize())
    {
        return false;
    }
    std::memcpy(outPath.GetData(), prefix.data(), prefix.size() * sizeof(wchar_t));
    std::memcpy(outPath.GetData() + prefix.size(), rest.data(), rest.size() * sizeof(wchar_t));
    outPath[prefix.size() + rest.size()] = L'\0';
    return true;
}

/// Extended-length UTF-16 path for a UTF-8 path.
[[nodiscard]] bool TryMakeWidePath(std::string_view path, WidePath& outPath) noexcept
{
    WidePath full;
    return GetFullPath(path, full) == true && MakeExtendedPath(full.GetData(), outPath) == true;
}

/// Strip the extended-length prefix GetFinalPathNameByHandleW returns: \\?\C:\x -> C:\x, \\?\UNC\s\x -> \\s\x.
[[nodiscard]] std::wstring_view StripExtendedPrefix(std::wstring_view path, WidePath& outScratch) noexcept
{
    if (path.substr(0, EXTENDED_UNC_PREFIX.size()) == EXTENDED_UNC_PREFIX)
    {
        const std::wstring_view rest = path.substr(EXTENDED_UNC_PREFIX.size());
        if (UNC_LEADING_SEPARATORS + rest.size() + 1 > outScratch.GetSize())
        {
            return {};
        }
        outScratch[0] = L'\\';
        outScratch[1] = L'\\';
        std::memcpy(outScratch.GetData() + UNC_LEADING_SEPARATORS, rest.data(), rest.size() * sizeof(wchar_t));
        return std::wstring_view(outScratch.GetData(), UNC_LEADING_SEPARATORS + rest.size());
    }
    if (path.substr(0, EXTENDED_PREFIX.size()) == EXTENDED_PREFIX)
    {
        return path.substr(EXTENDED_PREFIX.size());
    }
    return path;
}

/// Open an existing file or directory for metadata, following reparse points.
[[nodiscard]] HANDLE OpenForQuery(const wchar_t* path) noexcept
{
    return CreateFileW(path, 0, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

/// Final UTF-8 path of an open handle, with '/' separators and no extended prefix.
[[nodiscard]] bool GetFinalPath(HANDLE handle, NativePath& outPath) noexcept
{
    WidePath final;
    const DWORD units = GetFinalPathNameByHandleW(handle,
                                                  final.GetData(),
                                                  static_cast<DWORD>(final.GetSize()),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (units == 0 || units >= final.GetSize())
    {
        return false;
    }
    WidePath scratch;
    const std::wstring_view stripped = StripExtendedPrefix(std::wstring_view(final.GetData(), units), scratch);
    return stripped.empty() == false && EncodeUtf8(stripped, outPath) == true;
}

[[nodiscard]] bool IsDirectory(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(wide.GetData());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// Append the decimal digits of value.
[[nodiscard]] bool TryAppendDecimal(NativePath& inoutPath, uint64 value) noexcept
{
    constexpr usize MAX_DECIMAL_DIGITS = 20;
    constexpr uint64 DECIMAL_BASE = 10;
    foundation::StaticArray<char, MAX_DECIMAL_DIGITS> digits;
    usize count = 0;
    do
    {
        digits[MAX_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + value % DECIMAL_BASE);
        value /= DECIMAL_BASE;
        ++count;
    } while (value != 0);
    return inoutPath.TryAppend(std::string_view(digits.GetData() + MAX_DECIMAL_DIGITS - count, count)) ==
           foundation::StringStatus::Ok;
}

/// prefix + "<process>-<counter>": a name unique within this process run.
[[nodiscard]] bool TryMakeUniqueName(std::string_view prefix, NativePath& outPath) noexcept
{
    outPath.Clear();
    return outPath.TryAssign(prefix) == foundation::StringStatus::Ok &&
           TryAppendDecimal(outPath, GetCurrentProcessId()) == true &&
           outPath.TryAppend("-") == foundation::StringStatus::Ok &&
           TryAppendDecimal(outPath, gTemporaryCounter.fetch_add(1, std::memory_order_relaxed)) == true;
}

/// Create a new file for writing; INVALID_HANDLE_VALUE with the error left in GetLastError.
[[nodiscard]] HANDLE CreateNewFile(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wide.GetData(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
}

/// Create prefix<unique> beside the target and open it; false after MAX_TEMPORARY_ATTEMPTS collisions.
[[nodiscard]] bool CreateTemporaryFile(std::string_view prefix, NativePath& outPath, HANDLE& outFile) noexcept
{
    for (uint32 attempt = 0; attempt < MAX_TEMPORARY_ATTEMPTS; ++attempt)
    {
        if (TryMakeUniqueName(prefix, outPath) == false)
        {
            return false;
        }
        const HANDLE file = CreateNewFile(outPath.GetView());
        if (file != INVALID_HANDLE_VALUE)
        {
            outFile = file;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool WriteAll(HANDLE file, std::span<const uint8> data) noexcept
{
    usize written = 0;
    while (written < data.size())
    {
        const usize remaining = data.size() - written;
        const DWORD request = remaining < MAX_TRANSFER_BYTES ? static_cast<DWORD>(remaining) : MAX_TRANSFER_BYTES;
        DWORD count = 0;
        if (::WriteFile(file, data.data() + written, request, &count, nullptr) == FALSE || count == 0)
        {
            return false;
        }
        written += count;
    }
    return true;
}

[[nodiscard]] bool DeletePath(std::string_view path) noexcept
{
    WidePath wide;
    return TryMakeWidePath(path, wide) == true && DeleteFileW(wide.GetData()) != FALSE;
}

/// MoveFileExW replacing any existing file, as POSIX rename(2) and os.replace do.
[[nodiscard]] bool ReplacePath(std::string_view from, std::string_view to) noexcept
{
    WidePath source;
    WidePath destination;
    return TryMakeWidePath(from, source) == true && TryMakeWidePath(to, destination) == true &&
           MoveFileExW(source.GetData(), destination.GetData(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
               FALSE;
}

[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

/// Remove the directory named by inoutPath and its contents without following
/// reparse points; inoutPath is restored before returning.
[[nodiscard]] Status RemoveDirectoryContents(WidePath& inoutPath, usize length, uint32 depth) noexcept
{
    if (depth > MAX_REMOVE_DEPTH || length + SEARCH_SUFFIX_UNITS > inoutPath.GetSize())
    {
        return Status::IoError;
    }
    inoutPath[length] = L'\\';
    inoutPath[length + 1] = L'*';
    inoutPath[length + 2] = L'\0';
    WIN32_FIND_DATAW entry{};
    const HANDLE search =
        FindFirstFileExW(inoutPath.GetData(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    inoutPath[length] = L'\0';
    if (search == INVALID_HANDLE_VALUE)
    {
        return IsMissingError(GetLastError()) == true ? Status::Ok : Status::IoError;
    }
    Status status = Status::Ok;
    do
    {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..")
        {
            continue;
        }
        if (length + 1 + name.size() + 1 > inoutPath.GetSize())
        {
            status = Status::IoError;
            continue;
        }
        inoutPath[length] = L'\\';
        std::memcpy(inoutPath.GetData() + length + 1, name.data(), name.size() * sizeof(wchar_t));
        const usize childLength = length + 1 + name.size();
        inoutPath[childLength] = L'\0';
        const bool isDirectory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isReparsePoint = (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool isRemoved = false;
        if (isDirectory == true && isReparsePoint == false)
        {
            isRemoved = RemoveDirectoryContents(inoutPath, childLength, depth + 1) == Status::Ok &&
                        RemoveDirectoryW(inoutPath.GetData()) != FALSE;
        }
        else
        {
            // A directory symlink or junction is removed as a link, never followed.
            isRemoved = isDirectory == true ? RemoveDirectoryW(inoutPath.GetData()) != FALSE
                                            : DeleteFileW(inoutPath.GetData()) != FALSE;
        }
        if (isRemoved == false)
        {
            status = Status::IoError;
        }
        inoutPath[length] = L'\0';
    } while (FindNextFileW(search, &entry) != FALSE);
    if (GetLastError() != ERROR_NO_MORE_FILES)
    {
        status = Status::IoError;
    }
    (void)FindClose(search);
    return status;
}
} // namespace

Status ResolveDirectory(std::string_view path, foundation::String& outPath) noexcept
{
    WidePath full;
    WidePath wide;
    if (GetFullPath(path, full) == false || MakeExtendedPath(full.GetData(), wide) == false)
    {
        return Status::Invalid;
    }
    ScopedHandle handle(OpenForQuery(wide.GetData()));
    if (handle.IsValid() == false)
    {
        const DWORD error = GetLastError();
        return IsMissingError(error) == true || error == ERROR_INVALID_NAME ? Status::NotFound : Status::IoError;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.Get(), &information) == FALSE)
    {
        return Status::IoError;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return Status::NotFound;
    }
    NativePath resolved;
    if (GetFinalPath(handle.Get(), resolved) == false)
    {
        return Status::IoError;
    }
    return outPath.TryAssign(resolved.GetView()) == foundation::StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

Status MakeDirectories(std::string_view path) noexcept
{
    NativePath full;
    if (TryMakeNativePath(path, full) == false)
    {
        return Status::Invalid;
    }
    const std::string_view view = full.GetView();
    // The drive or share root always exists; CreateDirectoryW refuses it.
    const usize root = GetRootLength(view);
    NativePath prefix;
    for (usize i = root + 1; i <= view.size(); ++i)
    {
        if (i != view.size() && view[i] != '/')
        {
            continue;
        }
        WidePath wide;
        if (prefix.TryAssign(view.substr(0, i)) != foundation::StringStatus::Ok ||
            TryMakeWidePath(prefix.GetView(), wide) == false)
        {
            return Status::Invalid;
        }
        if (CreateDirectoryW(wide.GetData(), nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return Status::IoError;
        }
    }
    return IsDirectory(view) == true ? Status::Ok : Status::IoError;
}

Status WriteFileAtomic(std::string_view directory, std::string_view name, std::span<const uint8> data) noexcept
{
    NativePath target;
    NativePath prefix;
    if (target.TryAssign(directory) != foundation::StringStatus::Ok ||
        target.TryAppend("/") != foundation::StringStatus::Ok ||
        target.TryAppend(name) != foundation::StringStatus::Ok ||
        prefix.TryAssign(target.GetView()) != foundation::StringStatus::Ok ||
        prefix.TryAppend(".ludus-cook-") != foundation::StringStatus::Ok)
    {
        return Status::Limit;
    }
    NativePath temporary;
    HANDLE created = INVALID_HANDLE_VALUE;
    if (CreateTemporaryFile(prefix.GetView(), temporary, created) == false)
    {
        return Status::IoError;
    }
    ScopedHandle file(created);
    bool isOk = WriteAll(file.Get(), data) == true && FlushFileBuffers(file.Get()) != FALSE;
    isOk = file.Close() == true && isOk == true;
    if (isOk == false || ReplacePath(temporary.GetView(), target.GetView()) == false)
    {
        (void)DeletePath(temporary.GetView());
        return Status::IoError;
    }
    return Status::Ok;
}

Status ResolvePath(std::string_view path, foundation::String& outPath) noexcept
{
    if (path.empty() == true || path.find('\0') != std::string_view::npos || path.size() > MAX_NATIVE_PATH_BYTES)
    {
        return Status::Invalid;
    }
    WidePath full;
    if (GetFullPath(path, full) == false)
    {
        return Status::Invalid;
    }
    // ntpath.realpath(strict=False): resolve the longest existing prefix through
    // its final path and append the missing tail lexically.
    usize length = std::wstring_view(full.GetData()).size();
    usize tailBegin = 0;
    NativePath resolved;
    bool isResolved = false;
    while (length != 0)
    {
        const wchar_t saved = full[length];
        full[length] = L'\0';
        WidePath wide;
        if (MakeExtendedPath(full.GetData(), wide) == true)
        {
            ScopedHandle handle(OpenForQuery(wide.GetData()));
            isResolved = handle.IsValid() == true && GetFinalPath(handle.Get(), resolved) == true;
        }
        full[length] = saved;
        if (isResolved == true)
        {
            tailBegin = length;
            break;
        }
        // Drop the last component; keep the separator of a drive root ("C:\\") so
        // the root itself is tried. Without progress the whole path stays lexical.
        usize separator = length;
        while (separator > 0 && full[separator - 1] != L'\\')
        {
            --separator;
        }
        if (separator == 0)
        {
            break;
        }
        usize next = separator - 1;
        if (next > 0 && full[next - 1] == L':')
        {
            next = separator;
        }
        if (next >= length)
        {
            break;
        }
        length = next;
    }
    NativePath tail;
    const std::wstring_view rest = std::wstring_view(full.GetData() + tailBegin);
    if (rest.empty() == false && EncodeUtf8(rest, tail) == false)
    {
        return Status::Invalid;
    }
    if (isResolved == true)
    {
        // A drive root resolves to "C:/"; never double the separator.
        const std::string_view tailView =
            resolved.GetView().ends_with('/') == true && tail.GetView().starts_with('/') == true
                ? tail.GetView().substr(1)
                : tail.GetView();
        if (resolved.TryAppend(tailView) != foundation::StringStatus::Ok)
        {
            return Status::Limit;
        }
    }
    else
    {
        resolved = tail;
    }
    return outPath.TryAssign(resolved.GetView()) == foundation::StringStatus::Ok ? Status::Ok : Status::OutOfMemory;
}

bool DoesPathExist(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        return false;
    }
    // os.stat follows links: a dangling link does not exist.
    ScopedHandle handle(OpenForQuery(wide.GetData()));
    if (handle.IsValid() == true)
    {
        return true;
    }
    const DWORD error = GetLastError();
    return (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) &&
           GetFileAttributesW(wide.GetData()) != INVALID_FILE_ATTRIBUTES;
}

Status ReadFileBounded(std::string_view path, usize maximum, foundation::Array<uint8>& outData) noexcept
{
    SequentialFile file;
    const Status opened = file.Open(path);
    if (opened != Status::Ok)
    {
        return opened;
    }
    outData.Clear();
    foundation::StaticArray<uint8, NATIVE_READ_CHUNK_BYTES> buffer;
    while (true)
    {
        usize count = 0;
        const Status read = file.Read(std::span<uint8>(buffer.GetData(), buffer.GetSize()), count);
        if (read != Status::Ok)
        {
            return read;
        }
        if (count == 0)
        {
            return Status::Ok;
        }
        if (outData.GetSize() + count > maximum)
        {
            return Status::Limit;
        }
        if (outData.TryAddRange(std::span<const uint8>(buffer.GetData(), count)) == false)
        {
            return Status::OutOfMemory;
        }
    }
}

void SequentialFile::Close() noexcept
{
    if (mHandleOrNull != nullptr)
    {
        (void)CloseHandle(static_cast<HANDLE>(mHandleOrNull));
        mHandleOrNull = nullptr;
    }
}

Status SequentialFile::Open(std::string_view path) noexcept
{
    WidePath wide;
    if (mHandleOrNull != nullptr || TryMakeWidePath(path, wide) == false)
    {
        return Status::Invalid;
    }
    // Share delete and write access like a POSIX descriptor, so a concurrent
    // replacement (MoveFileExW) of the path still succeeds while it is read.
    const HANDLE file = CreateFileW(wide.GetData(),
                                    GENERIC_READ,
                                    SHARE_ALL,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return IsMissingError(GetLastError()) == true ? Status::NotFound : Status::IoError;
    }
    mHandleOrNull = file;
    return Status::Ok;
}

Status SequentialFile::Read(std::span<uint8> outBuffer, usize& outCount) noexcept
{
    outCount = 0;
    if (mHandleOrNull == nullptr)
    {
        return Status::IoError;
    }
    const DWORD request =
        outBuffer.size() < MAX_TRANSFER_BYTES ? static_cast<DWORD>(outBuffer.size()) : MAX_TRANSFER_BYTES;
    DWORD count = 0;
    if (::ReadFile(static_cast<HANDLE>(mHandleOrNull), outBuffer.data(), request, &count, nullptr) == FALSE)
    {
        return Status::IoError;
    }
    outCount = count;
    return Status::Ok;
}

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
    const std::string_view parent = GetParentDirectory(target.GetView());
    if (MakeDirectories(parent) != Status::Ok)
    {
        return PublishResult::IoError;
    }
    NativePath prefix;
    if (prefix.TryAssign(parent) != foundation::StringStatus::Ok ||
        (parent.ends_with('/') == false && prefix.TryAppend("/") != foundation::StringStatus::Ok) ||
        prefix.TryAppend(".pending-") != foundation::StringStatus::Ok)
    {
        return PublishResult::IoError;
    }
    NativePath temporary;
    HANDLE created = INVALID_HANDLE_VALUE;
    if (CreateTemporaryFile(prefix.GetView(), temporary, created) == false)
    {
        return PublishResult::IoError;
    }
    ScopedHandle file(created);
    PublishResult result = PublishResult::Ok;
    bool isWritten = WriteAll(file.Get(), data) == true && FlushFileBuffers(file.Get()) != FALSE;
    isWritten = file.Close() == true && isWritten == true;
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
        // os.link: CreateHardLinkW fails rather than replace an existing name.
        WidePath link;
        WidePath existingName;
        const bool isLinked = TryMakeWidePath(target.GetView(), link) == true &&
                              TryMakeWidePath(temporary.GetView(), existingName) == true &&
                              CreateHardLinkW(link.GetData(), existingName.GetData(), nullptr) != FALSE;
        const DWORD error = isLinked == true ? ERROR_SUCCESS : GetLastError();
        if (isLinked == false && error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
        {
            result = PublishResult::IoError;
        }
        else if (isLinked == false)
        {
            foundation::Array<uint8> existing;
            if (ReadFileBounded(target.GetView(), MAX_SOURCE_BYTES, existing) != Status::Ok)
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
    else if (ReplacePath(temporary.GetView(), target.GetView()) == false)
    {
        result = PublishResult::IoError;
    }
    // NOLINTEND(bugprone-branch-clone)
    if (result == PublishResult::Ok && SyncDirectory(parent) != Status::Ok)
    {
        result = PublishResult::IoError;
    }
    (void)DeletePath(temporary.GetView());
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
    return ReplacePath(source.GetView(), destination.GetView()) == true ? Status::Ok : Status::IoError;
}

Status RemoveFile(std::string_view path) noexcept
{
    NativePath native;
    if (TryMakeNativePath(path, native) == false)
    {
        return Status::Invalid;
    }
    return DeletePath(native.GetView()) == true ? Status::Ok : Status::IoError;
}

Status MakeDirectory(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        return Status::Invalid;
    }
    return CreateDirectoryW(wide.GetData(), nullptr) != FALSE ? Status::Ok : Status::IoError;
}

Status SyncDirectory(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        return Status::Invalid;
    }
    // Flushing a directory needs a writable directory handle (FlushFileBuffers).
    ScopedHandle directory(CreateFileW(wide.GetData(),
                                       GENERIC_WRITE,
                                       SHARE_ALL,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS,
                                       nullptr));
    if (directory.IsValid() == false)
    {
        return Status::IoError;
    }
    return FlushFileBuffers(directory.Get()) != FALSE ? Status::Ok : Status::IoError;
}

Status MakeTemporaryDirectory(std::string_view parent, std::string_view prefix, foundation::String& outPath) noexcept
{
    NativePath base;
    if (base.TryAssign(parent) != foundation::StringStatus::Ok || base.TryAppend("/") != foundation::StringStatus::Ok ||
        base.TryAppend(prefix) != foundation::StringStatus::Ok)
    {
        return Status::Limit;
    }
    for (uint32 attempt = 0; attempt < MAX_TEMPORARY_ATTEMPTS; ++attempt)
    {
        NativePath created;
        WidePath wide;
        if (TryMakeUniqueName(base.GetView(), created) == false || TryMakeWidePath(created.GetView(), wide) == false)
        {
            return Status::Limit;
        }
        if (CreateDirectoryW(wide.GetData(), nullptr) != FALSE)
        {
            return outPath.TryAssign(created.GetView()) == foundation::StringStatus::Ok ? Status::Ok
                                                                                        : Status::OutOfMemory;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return Status::IoError;
        }
    }
    return Status::IoError;
}

Status RemoveTree(std::string_view path) noexcept
{
    WidePath wide;
    if (TryMakeWidePath(path, wide) == false)
    {
        return Status::Invalid;
    }
    const DWORD attributes = GetFileAttributesW(wide.GetData());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return IsMissingError(GetLastError()) == true ? Status::Ok : Status::IoError;
    }
    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || isDirectory == false)
    {
        // A link is removed itself, never followed.
        const BOOL isRemoved = isDirectory == true ? RemoveDirectoryW(wide.GetData()) : DeleteFileW(wide.GetData());
        return isRemoved != FALSE ? Status::Ok : Status::IoError;
    }
    const usize length = std::wstring_view(wide.GetData()).size();
    const Status status = RemoveDirectoryContents(wide, length, 0);
    if (status != Status::Ok)
    {
        return status;
    }
    return RemoveDirectoryW(wide.GetData()) != FALSE ? Status::Ok : Status::IoError;
}

FileLock::~FileLock() noexcept
{
    if (mHandleOrNull != nullptr)
    {
        // Closing the handle releases its locks; unlock first so a waiter sees it at once.
        OVERLAPPED range{};
        range.Offset = LOCK_OFFSET_LOW;
        range.OffsetHigh = LOCK_OFFSET_HIGH;
        (void)UnlockFileEx(static_cast<HANDLE>(mHandleOrNull), 0, 1, 0, &range);
        (void)CloseHandle(static_cast<HANDLE>(mHandleOrNull));
    }
}

Status FileLock::Acquire(std::string_view path, LockMode mode) noexcept
{
    WidePath wide;
    if (mHandleOrNull != nullptr || TryMakeWidePath(path, wide) == false)
    {
        return Status::Invalid;
    }
    // Like the POSIX open(2) flags: an exclusive holder creates the file, a shared one requires it.
    const bool isExclusive = mode == LockMode::Exclusive;
    ScopedHandle file(CreateFileW(wide.GetData(),
                                  isExclusive == true ? FILE_APPEND_DATA : GENERIC_READ,
                                  SHARE_ALL,
                                  nullptr,
                                  isExclusive == true ? OPEN_ALWAYS : OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (file.IsValid() == false)
    {
        return Status::IoError;
    }
    OVERLAPPED range{};
    range.Offset = LOCK_OFFSET_LOW;
    range.OffsetHigh = LOCK_OFFSET_HIGH;
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (isExclusive == true ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (LockFileEx(file.Get(), flags, 0, 1, 0, &range) == FALSE)
    {
        return GetLastError() == ERROR_LOCK_VIOLATION ? Status::Conflict : Status::IoError;
    }
    mHandleOrNull = file.Release();
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::detail
