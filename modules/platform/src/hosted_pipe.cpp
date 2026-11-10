#include "internal/hosted_pipe.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/entropy.h>

#include <windows.h>

// Thanks to Microsoft, "Named Pipe Type, Read, and Wait Modes" and "ConnectNamedPipe",
// Win32 IPC reference, for byte-stream partial writes and the nonblocking listener.
// https://learn.microsoft.com/windows/win32/ipc/named-pipe-type-read-and-wait-modes
// https://learn.microsoft.com/windows/win32/api/namedpipeapi/nf-namedpipeapi-connectnamedpipe
namespace ludus::platform::hosted
{
namespace
{
using foundation::StaticArray;

constexpr std::string_view PIPE_PATH_PREFIX = "/ludus-viewport-";
constexpr std::string_view WIN32_PIPE_PREFIX = "\\\\.\\pipe\\";
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr usize PIPE_NONCE_BYTES = 16;
constexpr DWORD PIPE_BUFFER_BYTES = 64 * 1024;
constexpr DWORD TOKEN_USER_BYTES = sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE;
constexpr DWORD ACL_BYTES = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE;
constexpr uint8 HEX_DIGIT_BITS = 4;
constexpr uint8 HEX_DIGIT_MASK = 0x0f;
using PipeName = foundation::StaticString<WIN32_PIPE_PREFIX.size() + MAX_SOCKET_PATH_BYTES>;

struct TokenUserBuffer final
{
    alignas(TOKEN_USER) StaticArray<uint8, TOKEN_USER_BYTES> Bytes{};
};

[[nodiscard]] bool ReadUser(HANDLE process, TokenUserBuffer& outUser) noexcept
{
    HANDLE tokenOrNull = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &tokenOrNull) == FALSE)
    {
        return false;
    }
    DWORD size = 0;
    const BOOL hasUser =
        ::GetTokenInformation(tokenOrNull, TokenUser, outUser.Bytes.GetData(), TOKEN_USER_BYTES, &size);
    static_cast<void>(::CloseHandle(tokenOrNull));
    return hasUser != FALSE;
}

[[nodiscard]] PSID GetUserSid(TokenUserBuffer& inoutUser) noexcept
{
    return reinterpret_cast<TOKEN_USER*>(inoutUser.Bytes.GetData())->User.Sid;
}

[[nodiscard]] bool FormatPipeName(const SocketPath& path, PipeName& outName) noexcept
{
    const std::string_view text = path.GetView();
    if (text.starts_with(PIPE_PATH_PREFIX) == false || text.size() != PIPE_PATH_PREFIX.size() + 2 * PIPE_NONCE_BYTES)
    {
        return false;
    }
    for (const char digit : text.substr(PIPE_PATH_PREFIX.size()))
    {
        if (HEX_DIGITS.find(digit) == std::string_view::npos)
        {
            return false;
        }
    }
    return outName.TryAssign(WIN32_PIPE_PREFIX) == foundation::StringStatus::Ok &&
           outName.TryAppend(text.substr(1)) == foundation::StringStatus::Ok;
}

[[nodiscard]] TransferStatus GetTransferError() noexcept
{
    const DWORD error = ::GetLastError();
    if (error == ERROR_PIPE_BUSY)
    {
        return TransferStatus::WouldBlock;
    }
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
    {
        return TransferStatus::Closed;
    }
    return TransferStatus::Failed;
}
} // namespace

void SocketDescriptor::Reset(NativeStream descriptor) noexcept
{
    if (IsOpen() == true)
    {
        static_cast<void>(::CloseHandle(reinterpret_cast<HANDLE>(mDescriptor)));
    }
    mDescriptor = descriptor;
}

void ClosePeerProcess::operator()(void* processOrNull) const noexcept
{
    static_cast<void>(::CloseHandle(processOrNull));
}

bool MakePipePath(SocketPath& outPath) noexcept
{
    StaticArray<uint8, PIPE_NONCE_BYTES> nonce{};
    if (TryFillSystemEntropy(nonce.GetData(), nonce.GetSize()) == false ||
        outPath.TryAssign(PIPE_PATH_PREFIX) != foundation::StringStatus::Ok)
    {
        return false;
    }
    for (const uint8 byte : nonce)
    {
        const StaticArray<char, 2> digits{HEX_DIGITS[byte >> HEX_DIGIT_BITS], HEX_DIGITS[byte & HEX_DIGIT_MASK]};
        if (outPath.TryAppend({digits.GetData(), digits.GetSize()}) != foundation::StringStatus::Ok)
        {
            return false;
        }
    }
    return true;
}

SocketDescriptor ListenOnPipe(const SocketPath& path) noexcept
{
    PipeName name;
    TokenUserBuffer user;
    if (FormatPipeName(path, name) == false || ReadUser(::GetCurrentProcess(), user) == false)
    {
        return {};
    }
    alignas(ACL) StaticArray<uint8, ACL_BYTES> aclBytes{};
    ACL* acl = reinterpret_cast<ACL*>(aclBytes.GetData());
    SECURITY_DESCRIPTOR security{};
    if (::InitializeAcl(acl, ACL_BYTES, ACL_REVISION) == FALSE ||
        ::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, GetUserSid(user)) == FALSE ||
        ::InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
        ::SetSecurityDescriptorDacl(&security, TRUE, acl, FALSE) == FALSE)
    {
        return {};
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security, FALSE};
    const HANDLE pipe =
        ::CreateNamedPipeA(name.GetData(),
                           PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                           1,
                           PIPE_BUFFER_BYTES,
                           PIPE_BUFFER_BYTES,
                           0,
                           &attributes);
    return SocketDescriptor(reinterpret_cast<NativeStream>(pipe));
}

bool TryAcceptPipe(const SocketDescriptor& listener) noexcept
{
    const HANDLE pipe = reinterpret_cast<HANDLE>(listener.Get());
    if (::ConnectNamedPipe(pipe, nullptr) == FALSE)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NO_DATA)
        {
            static_cast<void>(::DisconnectNamedPipe(pipe));
        }
        return error == ERROR_PIPE_CONNECTED;
    }
    // The first nonblocking connect merely arms a listening instance.
    ULONG process = 0;
    return ::GetNamedPipeClientProcessId(pipe, &process) != FALSE;
}

SocketDescriptor ConnectToPipe(const SocketPath& path) noexcept
{
    PipeName name;
    if (FormatPipeName(path, name) == false)
    {
        return {};
    }
    const HANDLE pipe = ::CreateFileA(name.GetData(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                      nullptr);
    return SocketDescriptor(reinterpret_cast<NativeStream>(pipe));
}

PeerProcess OpenPipePeer(NativeStream descriptor, bool isServer) noexcept
{
    const HANDLE pipe = reinterpret_cast<HANDLE>(descriptor);
    ULONG processId = 0;
    const BOOL hasProcess = isServer == true ? ::GetNamedPipeClientProcessId(pipe, &processId)
                                             : ::GetNamedPipeServerProcessId(pipe, &processId);
    if (hasProcess == FALSE)
    {
        return {};
    }
    const DWORD rights = PROCESS_QUERY_LIMITED_INFORMATION | (isServer == true ? PROCESS_DUP_HANDLE : 0);
    PeerProcessHandle* processOrNull = static_cast<PeerProcessHandle*>(::OpenProcess(rights, FALSE, processId));
    PeerProcess peer(processOrNull);
    TokenUserBuffer localUser;
    TokenUserBuffer peerUser;
    if (peer.Get() == nullptr || ReadUser(::GetCurrentProcess(), localUser) == false ||
        ReadUser(peer.Get(), peerUser) == false || ::EqualSid(GetUserSid(localUser), GetUserSid(peerUser)) == FALSE)
    {
        return {};
    }
    return peer;
}

HostedImageHandle DuplicateImageHandle(const PeerProcess& peer, uint64 value) noexcept
{
    // DuplicateHandle accepts pseudo handles for the process and thread. Admit only
    // the positive 32-bit kernel handle values emitted by this protocol.
    constexpr uint64 MAX_KERNEL_HANDLE = 0x7fff'ffff;
    HANDLE duplicateOrNull = nullptr;
    if (value == 0 || value > MAX_KERNEL_HANDLE ||
        ::DuplicateHandle(peer.Get(),
                          reinterpret_cast<HANDLE>(value),
                          ::GetCurrentProcess(),
                          &duplicateOrNull,
                          0,
                          FALSE,
                          DUPLICATE_SAME_ACCESS) == FALSE)
    {
        return {};
    }
    return HostedImageHandle::FromWin32Handle(duplicateOrNull);
}

bool TryConfigureSocket(NativeStream descriptor) noexcept
{
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return ::SetNamedPipeHandleState(reinterpret_cast<HANDLE>(descriptor), &mode, nullptr, nullptr) != FALSE;
}

TransferStatus SendSome(NativeStream descriptor, foundation::BorrowedView<const uint8> bytes, usize& outSent) noexcept
{
    DWORD sent = 0;
    if (::WriteFile(reinterpret_cast<HANDLE>(descriptor),
                    bytes.GetDataOrNull(),
                    static_cast<DWORD>(bytes.GetSize()),
                    &sent,
                    nullptr) == FALSE)
    {
        return GetTransferError();
    }
    outSent = sent;
    return sent > 0 ? TransferStatus::Progress : TransferStatus::WouldBlock;
}

TransferStatus
ReceiveSome(NativeStream descriptor, foundation::BorrowedView<uint8> outBytes, usize& outReceived) noexcept
{
    DWORD available = 0;
    if (::PeekNamedPipe(reinterpret_cast<HANDLE>(descriptor), nullptr, 0, nullptr, &available, nullptr) == FALSE)
    {
        return GetTransferError();
    }
    if (available == 0)
    {
        return TransferStatus::WouldBlock;
    }
    DWORD received = 0;
    if (::ReadFile(reinterpret_cast<HANDLE>(descriptor),
                   outBytes.GetDataOrNull(),
                   static_cast<DWORD>(outBytes.GetSize()),
                   &received,
                   nullptr) == FALSE)
    {
        return GetTransferError();
    }
    outReceived = received;
    return received > 0 ? TransferStatus::Progress : TransferStatus::WouldBlock;
}

bool WaitUntilReadable(NativeStream descriptor, foundation::time::Duration timeout) noexcept
{
    const foundation::time::Timestamp start = foundation::time::Now();
    do
    {
        DWORD available = 0;
        if (::PeekNamedPipe(reinterpret_cast<HANDLE>(descriptor), nullptr, 0, nullptr, &available, nullptr) == FALSE ||
            available > 0)
        {
            return true;
        }
        ::Sleep(1);
    } while (foundation::time::Now().Nanoseconds - start.Nanoseconds < timeout.Nanoseconds);
    return false;
}
} // namespace ludus::platform::hosted
