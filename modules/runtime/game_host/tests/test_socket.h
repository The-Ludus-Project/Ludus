#pragma once

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <winsock2.h>
#    include <ws2tcpip.h>

#    include <windows.h>
#else
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

// A connected control stream pair for HostSession tests: an AF_UNIX socketpair on Linux
// and macOS, and a loopback TCP pair on Windows, which is what Python's socket.socketpair()
// gives a Windows play supervisor. Socket values travel as HostSession's int32 descriptor.
namespace ludus::runtime::game_host::tests
{
using foundation::int32;
using foundation::isize;
using foundation::uint8;
using foundation::usize;
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
[[nodiscard]] inline SOCKET ToSocket(int32 value) noexcept
{
    return static_cast<SOCKET>(static_cast<foundation::uint64>(static_cast<foundation::uint32>(value)));
}
[[nodiscard]] inline int32 FromSocket(SOCKET value) noexcept
{
    return static_cast<int32>(static_cast<foundation::uint32>(value));
}
#endif
/// Create a connected stream pair; outSockets[0] and [1] are interchangeable ends.
[[nodiscard]] inline bool MakeSocketPair(int32 (&outSockets)[2]) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    WSADATA data = {};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        return false;
    }
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    if (listener != INVALID_SOCKET && ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::listen(listener, 1) == 0 && ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0)
    {
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client != INVALID_SOCKET && ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
        {
            server = ::accept(listener, nullptr, nullptr);
        }
    }
    if (listener != INVALID_SOCKET)
    {
        (void)::closesocket(listener);
    }
    if (server == INVALID_SOCKET)
    {
        if (client != INVALID_SOCKET)
        {
            (void)::closesocket(client);
        }
        return false;
    }
    outSockets[0] = FromSocket(client);
    outSockets[1] = FromSocket(server);
    return true;
#else
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, outSockets) == 0;
#endif
}
/// Blocking write of up to size bytes; returns the count or a negative value on failure.
[[nodiscard]] inline isize WriteSocket(int32 socket, const uint8* bytes, usize size) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    return ::send(ToSocket(socket), reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0);
#else
    return ::write(socket, bytes, size);
#endif
}
/// Read available bytes; returns the count, 0 at end of stream or a negative value.
[[nodiscard]] inline isize ReadSocket(int32 socket, uint8* bytes, usize size) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    return ::recv(ToSocket(socket), reinterpret_cast<char*>(bytes), static_cast<int>(size), 0);
#else
    return ::read(socket, bytes, size);
#endif
}
/// Wait at most timeoutMilliseconds for readable bytes or end of stream.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] inline bool WaitReadable(int32 socket, int32 timeoutMilliseconds) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    WSAPOLLFD pending = {};
    pending.fd = ToSocket(socket);
    pending.events = POLLRDNORM;
    return ::WSAPoll(&pending, 1, timeoutMilliseconds) > 0 && (pending.revents & (POLLRDNORM | POLLHUP)) != 0;
#else
    pollfd pending = {};
    pending.fd = socket;
    pending.events = POLLIN;
    return ::poll(&pending, 1, timeoutMilliseconds) > 0 && (pending.revents & POLLIN) != 0;
#endif
}
/// Shut down both directions so a peer blocked in the session loop observes the end.
inline void ShutdownSocket(int32 socket) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    (void)::shutdown(ToSocket(socket), SD_BOTH);
#else
    (void)::shutdown(socket, SHUT_RDWR);
#endif
}
inline void CloseSocket(int32 socket) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    (void)::closesocket(ToSocket(socket));
#else
    (void)::close(socket);
#endif
}
/// Shrink the kernel send buffer to provoke output backpressure.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] inline bool SetSendBuffer(int32 socket, int32 bytes) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    return ::setsockopt(ToSocket(socket),
                        SOL_SOCKET,
                        SO_SNDBUF,
                        reinterpret_cast<const char*>(&bytes),
                        sizeof(bytes)) == 0;
#else
    return ::setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
#endif
}
} // namespace ludus::runtime::game_host::tests
