#include "worker_platform.hpp"

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "windows_pipe.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

#include <windows.h>

#include <tlhelp32.h>

// Thanks to Microsoft, Win32 documentation: "Named Pipe Server Using Overlapped I/O"
// (https://learn.microsoft.com/windows/win32/ipc/named-pipe-server-using-overlapped-i-o),
// "Named Pipe Security and Access Rights"
// (https://learn.microsoft.com/windows/win32/ipc/named-pipe-security-and-access-rights) and
// RegisterWaitForSingleObject. Adapted: a pool of overlapped instances plays the role of the
// Unix listen backlog; each connection is served to completion before the next is accepted.
// A private DACL grants only the current user, remote clients are rejected, and the first
// instance refuses a name another process already owns.
namespace ludus::tools::shader_worker
{
using namespace foundation;
namespace compiler = tooling::shader_compiler;
namespace
{
constexpr DWORD PIPE_BUFFER_BYTES = 64 * 1024;
constexpr usize LISTENER_COUNT = static_cast<usize>(CONNECTION_BACKLOG);
// ReadFile/WriteFile take 32-bit counts; larger frames continue in further operations.
constexpr usize MAX_TRANSFER_BYTES = usize{1} << 30U;
// Stops at once, even inside a Slang call: a compile has no safe interruption point. The
// system releases the pipe instances with the process; clients see a broken pipe.
[[noreturn]] void StopNow() noexcept
{
    static_cast<void>(::TerminateProcess(::GetCurrentProcess(), 0));
    ::ExitProcess(0);
}
BOOL WINAPI HandleControl(DWORD) noexcept
{
    StopNow();
}
VOID CALLBACK HandleParentExit(PVOID, BOOLEAN) noexcept
{
    StopNow();
}
// One overlapped operation at a time, bounded by IO_TIMEOUT_MILLISECONDS. A timed-out or
// failed operation is cancelled and awaited before its buffer can be released.
class PipeStream final : public Stream
{
public:
    explicit PipeStream(HANDLE event) noexcept : mEvent(event) {}
    /// Serve this connected instance next; the stream never owns the handle.
    void Attach(HANDLE pipe) noexcept
    {
        mPipe = pipe;
    }
    bool Transfer(uint8* bytes, usize size, bool isWrite) noexcept override
    {
        usize offset = 0;
        while (offset < size)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = mEvent;
            const usize remaining = size - offset;
            const DWORD count = static_cast<DWORD>(remaining < MAX_TRANSFER_BYTES ? remaining : MAX_TRANSFER_BYTES);
            const BOOL isComplete = isWrite == true ? ::WriteFile(mPipe, bytes + offset, count, nullptr, &overlapped)
                                                    : ::ReadFile(mPipe, bytes + offset, count, nullptr, &overlapped);
            if (isComplete == FALSE && ::GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }
            DWORD transferred = 0;
            if (::WaitForSingleObject(mEvent, static_cast<DWORD>(IO_TIMEOUT_MILLISECONDS)) != WAIT_OBJECT_0)
            {
                static_cast<void>(::CancelIoEx(mPipe, &overlapped));
                static_cast<void>(::GetOverlappedResult(mPipe, &overlapped, &transferred, TRUE));
                return false;
            }
            if (::GetOverlappedResult(mPipe, &overlapped, &transferred, FALSE) == FALSE || transferred == 0)
            {
                return false;
            }
            offset += static_cast<usize>(transferred);
        }
        return true;
    }

private:
    HANDLE mPipe = INVALID_HANDLE_VALUE;
    HANDLE mEvent = nullptr;
};
// Standard handles may be anonymous pipes, which do not support overlapped I/O, so the
// --stdio mode uses synchronous transfers that end with the owner's input.
class StandardStream final : public Stream
{
public:
    bool Transfer(uint8* bytes, usize size, bool isWrite) noexcept override
    {
        const HANDLE handle = ::GetStdHandle(isWrite == true ? STD_OUTPUT_HANDLE : STD_INPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        usize offset = 0;
        while (offset < size)
        {
            const usize remaining = size - offset;
            const DWORD count = static_cast<DWORD>(remaining < MAX_TRANSFER_BYTES ? remaining : MAX_TRANSFER_BYTES);
            DWORD transferred = 0;
            const BOOL isComplete = isWrite == true ? ::WriteFile(handle, bytes + offset, count, &transferred, nullptr)
                                                    : ::ReadFile(handle, bytes + offset, count, &transferred, nullptr);
            if (isComplete == FALSE || transferred == 0)
            {
                return false;
            }
            offset += static_cast<usize>(transferred);
        }
        return true;
    }
};
// Absolute security descriptor storage whose DACL grants only the current user.
struct PrivateSecurity final
{
    Array<uint8> UserBytes;
    Array<uint8> AclBytes;
    SECURITY_DESCRIPTOR Descriptor{};
    SECURITY_ATTRIBUTES Attributes{};
};
bool CreatePrivateSecurity(PrivateSecurity& outSecurity) noexcept
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return false;
    }
    DWORD userSize = 0;
    static_cast<void>(::GetTokenInformation(token, TokenUser, nullptr, 0, &userSize));
    const bool hasUser =
        userSize != 0 && outSecurity.UserBytes.TryResize(userSize) == true &&
        ::GetTokenInformation(token, TokenUser, outSecurity.UserBytes.GetData(), userSize, &userSize) != FALSE;
    static_cast<void>(::CloseHandle(token));
    if (hasUser == false)
    {
        return false;
    }
    PSID user = reinterpret_cast<const TOKEN_USER*>(outSecurity.UserBytes.GetData())->User.Sid;
    const DWORD aclSize =
        static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + ::GetLengthSid(user);
    if (::IsValidSid(user) == FALSE || outSecurity.AclBytes.TryResize(aclSize) == false)
    {
        return false;
    }
    PACL acl = reinterpret_cast<PACL>(outSecurity.AclBytes.GetData());
    if (::InitializeAcl(acl, aclSize, ACL_REVISION) == FALSE ||
        ::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user) == FALSE ||
        ::InitializeSecurityDescriptor(&outSecurity.Descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
        ::SetSecurityDescriptorDacl(&outSecurity.Descriptor, TRUE, acl, FALSE) == FALSE ||
        ::SetSecurityDescriptorControl(&outSecurity.Descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) == FALSE)
    {
        return false;
    }
    outSecurity.Attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    outSecurity.Attributes.lpSecurityDescriptor = &outSecurity.Descriptor;
    outSecurity.Attributes.bInheritHandle = FALSE;
    return true;
}
struct Listener final
{
    HANDLE Pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED Overlapped{};
    bool IsConnected = false;
};
HANDLE CreateInstance(const compiler::internal::PipeName& name, PrivateSecurity& inoutSecurity, bool isFirst) noexcept
{
    const DWORD openMode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (isFirst == true ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return ::CreateNamedPipeW(name.GetData(),
                              openMode,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES,
                              PIPE_BUFFER_BYTES,
                              PIPE_BUFFER_BYTES,
                              0,
                              &inoutSecurity.Attributes);
}
// Begin waiting for a client. A client that connected before the call is reported at once.
bool Arm(Listener& inoutListener, HANDLE event) noexcept
{
    inoutListener.Overlapped = OVERLAPPED{};
    inoutListener.Overlapped.hEvent = event;
    inoutListener.IsConnected = false;
    if (::ConnectNamedPipe(inoutListener.Pipe, &inoutListener.Overlapped) != FALSE)
    {
        return false;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
    {
        inoutListener.IsConnected = true;
        return ::SetEvent(event) != FALSE;
    }
    return error == ERROR_IO_PENDING;
}
bool FindParentProcess(DWORD& outParent) noexcept
{
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const DWORD self = ::GetCurrentProcessId();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool isFound = false;
    for (BOOL hasEntry = ::Process32FirstW(snapshot, &entry); hasEntry != FALSE && isFound == false;
         hasEntry = ::Process32NextW(snapshot, &entry))
    {
        if (entry.th32ProcessID == self)
        {
            outParent = entry.th32ParentProcessID;
            isFound = true;
        }
    }
    static_cast<void>(::CloseHandle(snapshot));
    return isFound;
}
bool GetCreationTime(HANDLE process, ULARGE_INTEGER& outTime) noexcept
{
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(process, &creation, &exit, &kernel, &user) == FALSE)
    {
        return false;
    }
    outTime.LowPart = creation.dwLowDateTime;
    outTime.HighPart = creation.dwHighDateTime;
    return true;
}
} // namespace
bool WriteVersion(std::string_view text) noexcept
{
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    return output != nullptr && output != INVALID_HANDLE_VALUE &&
           ::WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
           written == text.size();
}
bool InstallStopHandlers() noexcept
{
    return ::SetConsoleCtrlHandler(HandleControl, TRUE) != FALSE;
}
// A worker launched with --stop-with-parent stops when the process that started it exits,
// including a crash or TerminateProcess that never sends a console control event.
bool WatchParent() noexcept
{
    DWORD parentId = 0;
    if (FindParentProcess(parentId) == false || parentId == 0)
    {
        return false;
    }
    const HANDLE parent = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
    if (parent == nullptr)
    {
        // The parent already exited: its identifier no longer names a process.
        return false;
    }
    // Windows does not reparent processes, so a recorded parent identifier can be reused by a
    // later process. A genuine parent was created before this process.
    ULARGE_INTEGER parentCreation{};
    ULARGE_INTEGER selfCreation{};
    HANDLE wait = nullptr;
    if (GetCreationTime(parent, parentCreation) == false ||
        GetCreationTime(::GetCurrentProcess(), selfCreation) == false ||
        parentCreation.QuadPart > selfCreation.QuadPart ||
        ::RegisterWaitForSingleObject(&wait, parent, HandleParentExit, nullptr, INFINITE, WT_EXECUTEONLYONCE) == FALSE)
    {
        static_cast<void>(::CloseHandle(parent));
        return false;
    }
    // The wait keeps the parent handle for the life of the process; a parent that exited
    // before it was armed signals immediately and stops this worker.
    return true;
}
int32 ServeStandardStreams(compiler::Compiler& inoutCompiler) noexcept
{
    StandardStream stream;
    while (ProcessConnection(inoutCompiler, stream) == true)
    {
    }
    return 0;
}
int32 ServeEndpoint(compiler::Compiler& inoutCompiler, std::string_view endpoint) noexcept
{
    compiler::internal::PipeName name{};
    if (compiler::internal::TryConvertPipeName(endpoint, name) == false)
    {
        return EXIT_USAGE;
    }
    PrivateSecurity security;
    StaticArray<Listener, LISTENER_COUNT> listeners{};
    StaticArray<HANDLE, LISTENER_COUNT> events{};
    const HANDLE transferEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (transferEvent == nullptr || CreatePrivateSecurity(security) == false)
    {
        return EXIT_FAILURE_STATUS;
    }
    PipeStream stream{transferEvent};
    // Refuse an occupied name: never share an endpoint with another worker or process.
    for (usize i = 0; i < LISTENER_COUNT; ++i)
    {
        events[i] = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        listeners[i].Pipe = CreateInstance(name, security, i == 0);
        if (events[i] == nullptr || listeners[i].Pipe == INVALID_HANDLE_VALUE || Arm(listeners[i], events[i]) == false)
        {
            return EXIT_FAILURE_STATUS;
        }
    }
    while (true)
    {
        const DWORD signaled =
            ::WaitForMultipleObjects(static_cast<DWORD>(LISTENER_COUNT), events.GetData(), FALSE, INFINITE);
        if (signaled >= WAIT_OBJECT_0 + LISTENER_COUNT)
        {
            return EXIT_FAILURE_STATUS;
        }
        const usize index = static_cast<usize>(signaled - WAIT_OBJECT_0);
        Listener& listener = listeners[index];
        DWORD unused = 0;
        const bool isConnected = listener.IsConnected == true ||
                                 ::GetOverlappedResult(listener.Pipe, &listener.Overlapped, &unused, FALSE) != FALSE;
        static_cast<void>(::ResetEvent(events[index]));
        if (isConnected == true)
        {
            stream.Attach(listener.Pipe);
            static_cast<void>(ProcessConnection(inoutCompiler, stream));
        }
        // Close rather than disconnect: DisconnectNamedPipe discards a reply the client has not
        // read yet, while a closed server end still lets the client drain it. A fresh instance
        // keeps the backlog full.
        static_cast<void>(::CloseHandle(listener.Pipe));
        listener.Pipe = CreateInstance(name, security, false);
        if (listener.Pipe == INVALID_HANDLE_VALUE || Arm(listener, events[index]) == false)
        {
            return EXIT_FAILURE_STATUS;
        }
    }
}
} // namespace ludus::tools::shader_worker
