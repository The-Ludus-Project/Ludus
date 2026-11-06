#include "internal/control_channel.h"

#include <winsock2.h>

#include <windows.h>

// Thanks to Microsoft, Winsock documentation: "ioctlsocket" (FIONBIO), "send", "recv" and
// "WSAPoll" (https://learn.microsoft.com/windows/win32/api/winsock2/). The supervisor passes
// one end of a connected loopback stream pair as an inherited SOCKET handle; the host puts
// it in nonblocking mode and moves bytes without ever waiting inside a frame.
namespace ludus::runtime::game_host::control_channel
{
using foundation::int32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace
{
constexpr WORD WINSOCK_VERSION_2_2 = MAKEWORD(2, 2);
// send/recv take int counts; larger frames continue on later calls.
constexpr usize MAX_TRANSFER_BYTES = usize{1} << 30U;
[[nodiscard]] bool StartWinsock() noexcept
{
    // Winsock stays started for the process; the inherited socket outlives any session.
    static const bool isStarted = []() noexcept {
        WSADATA data = {};
        return ::WSAStartup(WINSOCK_VERSION_2_2, &data) == 0;
    }();
    return isStarted;
}
[[nodiscard]] SOCKET ToSocket(int32 channel) noexcept
{
    // Kernel handle values fit in 32 bits, so a SOCKET round-trips through --control-fd.
    return static_cast<SOCKET>(static_cast<uint64>(static_cast<foundation::uint32>(channel)));
}
[[nodiscard]] int ClampCount(usize size) noexcept
{
    return static_cast<int>(size < MAX_TRANSFER_BYTES ? size : MAX_TRANSFER_BYTES);
}
} // namespace
bool IsSupported() noexcept
{
    return true;
}
bool Open(int32 channel) noexcept
{
    u_long isNonblocking = 1;
    return channel >= 0 && StartWinsock() == true &&
           // FIONBIO is an unsigned control code for a signed parameter.
           ::ioctlsocket(ToSocket(channel), static_cast<long>(FIONBIO), &isNonblocking) == 0;
}
TransferStatus Send(int32 channel, const uint8* bytes, usize size, usize& outCount) noexcept
{
    outCount = 0;
    while (true)
    {
        const int count = ::send(ToSocket(channel), reinterpret_cast<const char*>(bytes), ClampCount(size), 0);
        if (count == SOCKET_ERROR)
        {
            const int error = ::WSAGetLastError();
            if (error == WSAEINTR)
            {
                continue;
            }
            return error == WSAEWOULDBLOCK ? TransferStatus::WouldBlock : TransferStatus::Failed;
        }
        if (count == 0)
        {
            return TransferStatus::WouldBlock;
        }
        outCount = static_cast<usize>(count);
        return TransferStatus::Transferred;
    }
}
TransferStatus Receive(int32 channel, uint8* bytes, usize size, usize& outCount) noexcept
{
    outCount = 0;
    const int count = ::recv(ToSocket(channel), reinterpret_cast<char*>(bytes), ClampCount(size), 0);
    if (count == 0)
    {
        return TransferStatus::Closed;
    }
    if (count == SOCKET_ERROR)
    {
        // A loopback peer that closes with unread data resets the connection: that is the
        // supervisor's end of the session, as end-of-stream is on a Unix socket.
        const int error = ::WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR ? TransferStatus::WouldBlock : TransferStatus::Closed;
    }
    outCount = static_cast<usize>(count);
    return TransferStatus::Transferred;
}
// The channel and timeout are distinct roles of one fixed signature.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void WaitWritable(int32 channel, int32 timeoutMilliseconds) noexcept
{
    WSAPOLLFD pending = {};
    pending.fd = ToSocket(channel);
    pending.events = POLLWRNORM;
    (void)::WSAPoll(&pending, 1, timeoutMilliseconds);
}
uint64 GetCurrentProcessIdentifier() noexcept
{
    return static_cast<uint64>(::GetCurrentProcessId());
}
} // namespace ludus::runtime::game_host::control_channel
