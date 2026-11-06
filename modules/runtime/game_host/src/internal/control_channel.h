#pragma once

#include <ludus/foundation/base/types.h>

// Native byte transport between a HostSession and its play supervisor (design 10).
// Linux and macOS use the inherited nonblocking stream socket named by --control-fd.
// Windows has no supervisor transport yet (D6c): sessions run without a control
// channel, and a control descriptor is refused before a session starts.
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
