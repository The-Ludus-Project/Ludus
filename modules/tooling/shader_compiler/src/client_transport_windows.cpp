#include "client_transport.hpp"
#include "windows_pipe.hpp"

#include <new>

#include <windows.h>

// Thanks to Microsoft, "Named Pipe Client" and "Synchronization and Overlapped Input and
// Output" (Win32 documentation, https://learn.microsoft.com/windows/win32/ipc/named-pipe-client,
// https://learn.microsoft.com/windows/win32/sync/synchronization-and-overlapped-input-and-output):
// the client opens an existing instance without waiting and polls one overlapped operation
// at a time, so a render or editor thread never blocks on the worker.
namespace ludus::tooling::shader_compiler::internal
{
using namespace foundation;
namespace
{
// ReadFile/WriteFile take 32-bit counts; larger frames continue in further operations.
constexpr usize MAX_TRANSFER_BYTES = usize{1} << 30U;
// UTF-8 needs at most three bytes for one UTF-16 code unit of a pipe name.
constexpr usize MAX_UTF8_BYTES_PER_CHARACTER = 3;
constexpr usize MAX_PIPE_NAME_BYTES = MAX_PIPE_NAME_CHARACTERS * MAX_UTF8_BYTES_PER_CHARACTER;
} // namespace
bool TryConvertPipeName(std::string_view endpoint, PipeName& outName) noexcept
{
    if (endpoint.size() <= PIPE_PREFIX.size() || endpoint.substr(0, PIPE_PREFIX.size()) != PIPE_PREFIX ||
        endpoint.size() > MAX_PIPE_NAME_BYTES || endpoint.find('\0') != std::string_view::npos ||
        endpoint.find('\\', PIPE_PREFIX.size()) != std::string_view::npos)
    {
        return false;
    }
    const int32 count = ::MultiByteToWideChar(CP_UTF8,
                                              MB_ERR_INVALID_CHARS,
                                              endpoint.data(),
                                              static_cast<int32>(endpoint.size()),
                                              outName.GetData(),
                                              static_cast<int32>(MAX_PIPE_NAME_CHARACTERS));
    if (count <= 0)
    {
        return false;
    }
    outName[static_cast<usize>(count)] = L'\0';
    return true;
}
struct ClientConnection::Native final
{
    HANDLE Pipe = INVALID_HANDLE_VALUE;
    HANDLE Event = nullptr;
    OVERLAPPED Overlapped{};
    bool IsPending = false;

    Status Transfer(uint8* bytes, usize size, usize& inoutOffset, bool isWrite) noexcept
    {
        while (inoutOffset < size)
        {
            if (IsPending == false)
            {
                Overlapped = OVERLAPPED{};
                Overlapped.hEvent = Event;
                const usize remaining = size - inoutOffset;
                const DWORD count = static_cast<DWORD>(remaining < MAX_TRANSFER_BYTES ? remaining : MAX_TRANSFER_BYTES);
                const BOOL isComplete = isWrite == true
                                            ? ::WriteFile(Pipe, bytes + inoutOffset, count, nullptr, &Overlapped)
                                            : ::ReadFile(Pipe, bytes + inoutOffset, count, nullptr, &Overlapped);
                if (isComplete == FALSE && ::GetLastError() != ERROR_IO_PENDING)
                {
                    return Status::TransportFailed;
                }
                // A synchronous completion is still reported through the overlapped result.
                IsPending = true;
            }
            DWORD transferred = 0;
            if (::GetOverlappedResult(Pipe, &Overlapped, &transferred, FALSE) == FALSE)
            {
                if (::GetLastError() == ERROR_IO_INCOMPLETE)
                {
                    return Status::Pending;
                }
                IsPending = false;
                return Status::TransportFailed;
            }
            IsPending = false;
            if (transferred == 0)
            {
                return Status::TransportFailed;
            }
            inoutOffset += static_cast<usize>(transferred);
        }
        return Status::Ready;
    }
};
ClientConnection::ClientConnection() noexcept = default;
ClientConnection::~ClientConnection() noexcept
{
    Close();
}
void ClientConnection::Close() noexcept
{
    if (mNative.Get() == nullptr)
    {
        return;
    }
    Native& native = *mNative;
    if (native.IsPending == true)
    {
        // The kernel may still write into the request buffers; wait for the cancellation.
        static_cast<void>(::CancelIoEx(native.Pipe, &native.Overlapped));
        DWORD transferred = 0;
        static_cast<void>(::GetOverlappedResult(native.Pipe, &native.Overlapped, &transferred, TRUE));
        native.IsPending = false;
    }
    if (native.Pipe != INVALID_HANDLE_VALUE)
    {
        static_cast<void>(::CloseHandle(native.Pipe));
    }
    if (native.Event != nullptr)
    {
        static_cast<void>(::CloseHandle(native.Event));
    }
    mNative.Reset();
}
Status ClientConnection::Open(BorrowedView<const char> endpoint) noexcept
{
    Close();
    PipeName name{};
    if (endpoint.IsValid() == false ||
        TryConvertPipeName({endpoint.GetDataOrNull(), endpoint.GetSize()}, name) == false)
    {
        return Status::InvalidRequest;
    }
    Native* allocationOrNull = new (std::nothrow) Native;
    core::UniquePtr<Native> native{allocationOrNull};
    if (native.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    native->Event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (native->Event == nullptr)
    {
        return Status::Unavailable;
    }
    // Identification-level impersonation only: the worker cannot act as this user. The handle
    // is not inherited. A busy or missing worker is reported at once rather than awaited.
    native->Pipe = ::CreateFileW(name.GetData(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                 nullptr);
    if (native->Pipe == INVALID_HANDLE_VALUE)
    {
        static_cast<void>(::CloseHandle(native->Event));
        return Status::Unavailable;
    }
    mNative = Move(native);
    return Status::Ready;
}
Status ClientConnection::Send(const uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    if (mNative.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    // WriteFile does not modify the source; the cast only shares Transfer with ReadFile.
    return mNative->Transfer(const_cast<uint8*>(bytes), size, inoutOffset, true);
}
Status ClientConnection::Receive(uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    if (mNative.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    return mNative->Transfer(bytes, size, inoutOffset, false);
}
} // namespace ludus::tooling::shader_compiler::internal
