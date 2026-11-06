#pragma once

#include <ludus/foundation/base/types.h>

// Native byte transport between a HostSession and its play supervisor (design 10).
// The supervisor passes one end of a connected stream socket pair, named by --control-fd:
// a Unix-domain socket descriptor on Linux and macOS, and an inherited Winsock SOCKET
// handle on Windows (Python's socket.socketpair() is a loopback TCP pair there, D6c).
namespace ludus::runtime::game_host::control_channel
{
/// Outcome of one nonblocking transfer.
enum class TransferStatus : foundation::uint8
{
    Transferred, ///< outCount bytes moved; may be fewer than requested.
    WouldBlock,  ///< Nothing can move now; retry on a later frame.
    Closed,      ///< The peer closed the channel (receive only).
    Failed,      ///< The channel is unusable.
};
/// True when this platform has a supervisor transport for control descriptors.
[[nodiscard]] bool IsSupported() noexcept;
/// Prepare an inherited channel for nonblocking use before the first transfer.
/// Windows starts Winsock in this process and selects nonblocking mode; elsewhere this
/// checks nothing. A false result means the session must end without a channel.
[[nodiscard]] bool Open(foundation::int32 channel) noexcept;
/// Send without blocking. Interrupted calls are retried.
[[nodiscard]] TransferStatus Send(foundation::int32 channel,
                                  const foundation::uint8* bytes,
                                  foundation::usize size,
                                  foundation::usize& outCount) noexcept;
/// Receive bytes that are already available, without blocking.
[[nodiscard]] TransferStatus Receive(foundation::int32 channel,
                                     foundation::uint8* bytes,
                                     foundation::usize size,
                                     foundation::usize& outCount) noexcept;
/// Wait at most timeoutMilliseconds for send capacity.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void WaitWritable(foundation::int32 channel, foundation::int32 timeoutMilliseconds) noexcept;
/// Identifier of the current process, mixed into session identities.
[[nodiscard]] foundation::uint64 GetCurrentProcessIdentifier() noexcept;
} // namespace ludus::runtime::game_host::control_channel
