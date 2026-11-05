#pragma once

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <windows.h>
#include <winternl.h>

namespace ludus::foundation::filesystem::windows
{
constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD MAX_TRANSFER_BYTES = 0x7ffff000;
constexpr std::wstring_view LOCK_NAME = L".ludus-write-lock";
using Component = StaticArray<wchar_t, MAX_PATH_BYTES + 1>;

class NativeHandle final
{
public:
    NativeHandle() noexcept = default;
    ~NativeHandle() noexcept;
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept;
    [[nodiscard]] HANDLE Release() noexcept;
    void Reset(HANDLE handle) noexcept;
    [[nodiscard]] Result Close() noexcept;

private:
    HANDLE mHandle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] Result MapError(DWORD error) noexcept;
[[nodiscard]] Result OpenRoot(std::string_view path, NativeHandle& outRoot) noexcept;
[[nodiscard]] Result OpenChild(HANDLE parent,
                               std::wstring_view name,
                               ACCESS_MASK access,
                               ULONG sharing,
                               ULONG disposition,
                               ULONG options,
                               NativeHandle& outChild) noexcept;
[[nodiscard]] Result
ResolveParent(HANDLE root, std::string_view path, NativeHandle& outParent, Component& outLeaf) noexcept;
[[nodiscard]] Result
OpenRegular(HANDLE parent, std::wstring_view name, ACCESS_MASK access, ULONG options, NativeHandle& outFile) noexcept;
[[nodiscard]] Result ObserveFile(HANDLE file, FileStamp& outStamp) noexcept;
} // namespace ludus::foundation::filesystem::windows
