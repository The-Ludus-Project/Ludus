#include "internal/windows_files.hpp"

#include <ludus/foundation/base/core.h>

namespace ludus::foundation::filesystem::windows
{
namespace
{
constexpr int64 TICKS_PER_SECOND = 10'000'000;
constexpr int64 NANOSECONDS_PER_TICK = 100;
constexpr uint32 NATIVE_OFFSET_WORD_BITS = 32;

bool DecodeName(std::string_view name, wchar_t* outName, usize capacity) noexcept
{
    if (name.empty() == true || name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    const int32 units = MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            name.data(),
                                            static_cast<int32>(name.size()),
                                            outName,
                                            static_cast<int32>(capacity - 1));
    if (units == 0)
    {
        return false;
    }
    outName[units] = L'\0';
    return true;
}

Result CheckType(HANDLE handle, bool isDirectory, bool shouldRejectReparse) noexcept
{
    if (GetFileType(handle) != FILE_TYPE_DISK)
    {
        return {Status::NotRegularFile};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE)
    {
        return MapError(GetLastError());
    }
    if (shouldRejectReparse == true && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return {Status::AccessDenied};
    }
    const bool isActualDirectory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isActualDirectory != isDirectory)
    {
        return {isDirectory == true ? Status::InvalidArgument : Status::NotRegularFile};
    }
    return {};
}

bool DecodeComponent(std::string_view name, Component& outName) noexcept
{
    // Keep names addressable through ordinary Win32 paths as well as the NT
    // relative API. In particular, do not let trailing dots/spaces alias a leaf.
    return name.empty() == false && name.back() != '.' && name.back() != ' ' &&
           name.find_first_of("<>\"|?*") == std::string_view::npos &&
           DecodeName(name, outName.GetData(), outName.GetSize()) == true &&
           CompareStringOrdinal(outName.GetData(), -1, LOCK_NAME.data(), -1, TRUE) != CSTR_EQUAL;
}
} // namespace

NativeHandle::~NativeHandle() noexcept
{
    (void)Close();
}

HANDLE NativeHandle::Get() const noexcept
{
    return mHandle;
}

HANDLE NativeHandle::Release() noexcept
{
    const HANDLE handle = mHandle;
    mHandle = INVALID_HANDLE_VALUE;
    return handle;
}

void NativeHandle::Reset(HANDLE handle) noexcept
{
    (void)Close();
    mHandle = handle;
}

Result NativeHandle::Close() noexcept
{
    const HANDLE handle = Release();
    return handle != INVALID_HANDLE_VALUE && CloseHandle(handle) == FALSE ? MapError(GetLastError()) : Result{};
}

Result MapError(DWORD error) noexcept
{
    Status code = Status::IoError;
    switch (error)
    {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            code = Status::NotFound;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
        case ERROR_CANT_ACCESS_FILE:
            code = Status::AccessDenied;
            break;
        case ERROR_INVALID_NAME:
        case ERROR_DIRECTORY:
        case ERROR_FILENAME_EXCED_RANGE:
            code = Status::InvalidArgument;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            code = Status::OutOfMemory;
            break;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            code = Status::Unsupported;
            break;
        default:
            break;
    }
    return {code, static_cast<int32>(error)};
}

Result OpenRoot(std::string_view path, NativeHandle& outRoot) noexcept
{
    StaticArray<wchar_t, MAX_ROOT_BYTES + 1> name;
    if (path.size() > MAX_ROOT_BYTES || DecodeName(path, name.GetData(), name.GetSize()) == false)
    {
        return {Status::InvalidArgument};
    }
    NativeHandle root;
    root.Reset(CreateFileW(name.GetData(),
                           FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                           SHARE_ALL,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr));
    if (root.Get() == INVALID_HANDLE_VALUE)
    {
        return MapError(GetLastError());
    }
    const Result result = CheckType(root.Get(), true, false);
    if (result.Succeeded() == true)
    {
        outRoot.Reset(root.Release());
    }
    return result;
}

Result OpenChild(HANDLE parent,
                 std::wstring_view name,
                 ACCESS_MASK access,
                 ULONG sharing,
                 ULONG disposition,
                 ULONG options,
                 NativeHandle& outChild) noexcept
{
    // Thanks to Microsoft, "NtCreateFile function (winternl.h)", for opening
    // one component relative to a pinned directory handle without reparse traversal.
    // https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntcreatefile
    UNICODE_STRING nativeName{};
    nativeName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    nativeName.MaximumLength = nativeName.Length;
    nativeName.Buffer = const_cast<wchar_t*>(name.data()); // NT's input-only name ABI takes a mutable pointer.
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &nativeName, OBJ_CASE_INSENSITIVE, parent, nullptr);
    IO_STATUS_BLOCK operation{};
    HANDLE childOrNull = nullptr;
    const NTSTATUS status = NtCreateFile(&childOrNull,
                                         access | SYNCHRONIZE,
                                         &attributes,
                                         &operation,
                                         nullptr,
                                         FILE_ATTRIBUTE_NORMAL,
                                         sharing,
                                         disposition,
                                         options | FILE_OPEN_REPARSE_POINT,
                                         nullptr,
                                         0);
    if (status < 0)
    {
        return MapError(RtlNtStatusToDosError(status));
    }
    outChild.Reset(childOrNull);
    return {};
}

Result ResolveParent(HANDLE root, std::string_view path, NativeHandle& outParent, Component& outLeaf) noexcept
{
    if (ValidPath(path) == false)
    {
        return {Status::InvalidArgument};
    }
    HANDLE parent = root;
    usize begin = 0;
    while (true)
    {
        const usize slash = path.find('/', begin);
        const usize end = slash == std::string_view::npos ? path.size() : slash;
        if (DecodeComponent(path.substr(begin, end - begin), outLeaf) == false)
        {
            return {Status::InvalidArgument};
        }
        if (slash == std::string_view::npos)
        {
            return {};
        }
        NativeHandle child;
        Result result = OpenChild(parent,
                                  outLeaf.GetData(),
                                  FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                  SHARE_ALL,
                                  FILE_OPEN,
                                  FILE_SYNCHRONOUS_IO_NONALERT,
                                  child);
        if (result.Succeeded() == false)
        {
            return result;
        }
        result = CheckType(child.Get(), true, true);
        if (result.Succeeded() == false)
        {
            return result;
        }
        outParent.Reset(child.Release());
        parent = outParent.Get();
        begin = slash + 1;
    }
}

Result
OpenRegular(HANDLE parent, std::wstring_view name, ACCESS_MASK access, ULONG options, NativeHandle& outFile) noexcept
{
    const Result result =
        OpenChild(parent, name, access | FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, options, outFile);
    return result.Succeeded() == true ? CheckType(outFile.Get(), false, true) : result;
}

Result ObserveFile(HANDLE file, FileStamp& outStamp) noexcept
{
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO times{};
    if (GetFileInformationByHandle(file, &identity) == FALSE ||
        GetFileInformationByHandleEx(file, FileBasicInfo, &times, sizeof(times)) == FALSE)
    {
        return MapError(GetLastError());
    }
    const int64 modified = times.LastWriteTime.QuadPart;
    const int64 changed = times.ChangeTime.QuadPart;
    outStamp =
    {
        .Device = identity.dwVolumeSerialNumber,
        .Inode = (static_cast<uint64>(identity.nFileIndexHigh) << NATIVE_OFFSET_WORD_BITS) | identity.nFileIndexLow,
        .Size = (static_cast<uint64>(identity.nFileSizeHigh) << NATIVE_OFFSET_WORD_BITS) | identity.nFileSizeLow,
        .ModifiedSeconds = modified / TICKS_PER_SECOND,
        .ModifiedNanoseconds = (modified % TICKS_PER_SECOND) * NANOSECONDS_PER_TICK,
        .ChangedSeconds = changed / TICKS_PER_SECOND,
        .ChangedNanoseconds = (changed % TICKS_PER_SECOND) * NANOSECONDS_PER_TICK,
    };
    return {};
}
} // namespace ludus::foundation::filesystem::windows
