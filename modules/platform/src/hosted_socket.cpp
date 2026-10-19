#include "internal/hosted_socket.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(__APPLE__) && !defined(__linux__)
#    error "The hosted window channel requires macOS or Linux"
#endif

namespace ludus::platform::hosted
{
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
#if defined(__APPLE__)
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
#if defined(__linux__)
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

bool WaitUntilReadable(int32 descriptor, uint32 timeoutMilliseconds) noexcept
{
    pollfd request{};
    request.fd = descriptor;
    request.events = POLLIN;
    while (true)
    {
        const int32 count = ::poll(&request, 1, static_cast<int32>(timeoutMilliseconds));
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        return count > 0;
    }
}

bool TryReadPeerUserId(int32 descriptor, uint32& outUserId) noexcept
{
#if defined(__APPLE__)
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
