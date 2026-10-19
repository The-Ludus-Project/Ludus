#include "internal/hosted_socket.hpp"

#include <ludus/foundation/base/config.h>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if LUDUS_TARGET_OS != LUDUS_OS_MACOS && LUDUS_TARGET_OS != LUDUS_OS_LINUX
#    error "The hosted window channel requires macOS or Linux"
#endif

namespace ludus::platform::hosted
{
namespace
{
constexpr foundation::uint64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
/// Largest timeout poll(2) accepts: INT_MAX milliseconds.
constexpr foundation::uint64 MAX_POLL_MILLISECONDS = 2'147'483'647;
} // namespace

void SocketDescriptor::Reset(int32 descriptor) noexcept
{
    if (mDescriptor >= 0)
    {
        static_cast<void>(::close(mDescriptor));
    }
    mDescriptor = descriptor;
}

bool TryConfigureSocket(int32 descriptor) noexcept
{
    const int32 flags = ::fcntl(descriptor, F_GETFL, 0);
    const int32 descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
    if (flags < 0 || descriptorFlags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
    {
        return false;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    const int32 shouldSuppressPipeSignal = 1;
    if (::setsockopt(descriptor,
                     SOL_SOCKET,
                     SO_NOSIGPIPE,
                     &shouldSuppressPipeSignal,
                     sizeof(shouldSuppressPipeSignal)) != 0)
    {
        return false;
    }
#endif
    return true;
}

TransferStatus SendSome(int32 descriptor, foundation::BorrowedView<const uint8> bytes, usize& outSent) noexcept
{
    if (bytes.IsValid() == false)
    {
        return TransferStatus::Failed;
    }
    while (true)
    {
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
        const ssize_t count = ::send(descriptor, bytes.GetDataOrNull(), bytes.GetSize(), MSG_NOSIGNAL);
#else
        const ssize_t count = ::send(descriptor, bytes.GetDataOrNull(), bytes.GetSize(), 0);
#endif
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return TransferStatus::WouldBlock;
        }
        if (count < 0 && (errno == EPIPE || errno == ECONNRESET))
        {
            return TransferStatus::Closed;
        }
        if (count < 0)
        {
            return TransferStatus::Failed;
        }
        outSent = static_cast<usize>(count);
        return TransferStatus::Progress;
    }
}

TransferStatus ReceiveSome(int32 descriptor, foundation::BorrowedView<uint8> outBytes, usize& outReceived) noexcept
{
    if (outBytes.IsValid() == false || outBytes.GetSize() == 0)
    {
        return TransferStatus::Failed;
    }
    while (true)
    {
        const ssize_t count = ::recv(descriptor, outBytes.GetDataOrNull(), outBytes.GetSize(), 0);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return TransferStatus::WouldBlock;
        }
        if (count == 0 || (count < 0 && errno == ECONNRESET))
        {
            return TransferStatus::Closed;
        }
        if (count < 0)
        {
            return TransferStatus::Failed;
        }
        outReceived = static_cast<usize>(count);
        return TransferStatus::Progress;
    }
}

bool WaitUntilReadable(int32 descriptor, foundation::time::Duration timeout) noexcept
{
    const foundation::uint64 milliseconds =
        (timeout.Nanoseconds + NANOSECONDS_PER_MILLISECOND - 1) / NANOSECONDS_PER_MILLISECOND;
    const int32 timeoutMilliseconds =
        static_cast<int32>(milliseconds < MAX_POLL_MILLISECONDS ? milliseconds : MAX_POLL_MILLISECONDS);
    pollfd request{};
    request.fd = descriptor;
    request.events = POLLIN;
    while (true)
    {
        const int32 count = ::poll(&request, 1, timeoutMilliseconds);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        return count > 0;
    }
}

bool TryReadPeerUserId(int32 descriptor, uint32& outUserId) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    uid_t userId = 0;
    gid_t groupId = 0;
    if (::getpeereid(descriptor, &userId, &groupId) != 0)
    {
        return false;
    }
    outUserId = static_cast<uint32>(userId);
    return true;
#else
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials))
    {
        return false;
    }
    outUserId = static_cast<uint32>(credentials.uid);
    return true;
#endif
}

uint32 GetCurrentUserId() noexcept
{
    return static_cast<uint32>(::geteuid());
}
} // namespace ludus::platform::hosted
