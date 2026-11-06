#include "internal/control_channel.h"

#include <cerrno>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ludus::runtime::game_host::control_channel
{
using foundation::int32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
bool IsSupported() noexcept
{
    return true;
}
TransferStatus Send(int32 channel, const uint8* bytes, usize size, usize& outCount) noexcept
{
    outCount = 0;
    while (true)
    {
        const ssize_t count = ::send(channel, bytes, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK ? TransferStatus::WouldBlock : TransferStatus::Failed;
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
    // Non-blocking poll so the frame loop is never starved.
    pollfd pending = {};
    pending.fd = channel;
    pending.events = POLLIN;
    const int ready = ::poll(&pending, 1, 0);
    if (ready <= 0 || (pending.revents & (POLLIN | POLLHUP)) == 0)
    {
        return TransferStatus::WouldBlock;
    }
    const ssize_t count = ::recv(channel, bytes, size, MSG_DONTWAIT);
    if (count == 0)
    {
        return TransferStatus::Closed;
    }
    if (count < 0)
    {
        return TransferStatus::WouldBlock;
    }
    outCount = static_cast<usize>(count);
    return TransferStatus::Transferred;
}
// The channel and timeout are distinct roles of one fixed signature.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void WaitWritable(int32 channel, int32 timeoutMilliseconds) noexcept
{
    pollfd pending = {channel, POLLOUT, 0};
    (void)::poll(&pending, 1, timeoutMilliseconds);
}
uint64 GetCurrentProcessIdentifier() noexcept
{
    return static_cast<uint64>(::getpid());
}
} // namespace ludus::runtime::game_host::control_channel
