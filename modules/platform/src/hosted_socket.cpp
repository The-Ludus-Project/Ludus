#include "internal/hosted_socket.hpp"

#include <ludus/foundation/containers/static_array.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#if defined(__APPLE__)
#    include <sys/un.h>
#endif
#include <unistd.h>

#if !defined(__APPLE__) && !defined(__linux__)
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

TransferStatus SendWithDescriptors(int32 descriptor,
                                   foundation::BorrowedView<const uint8> bytes,
                                   const int32* descriptors,
                                   usize descriptorCount,
                                   usize& outSent) noexcept
{
    if (bytes.IsValid() == false || bytes.GetSize() == 0 || descriptors == nullptr || descriptorCount == 0 ||
        descriptorCount > MAX_TRANSFER_DESCRIPTORS)
    {
        return TransferStatus::Failed;
    }
    // Aligned control buffer for the largest SCM_RIGHTS message this channel sends.
    alignas(cmsghdr) foundation::StaticArray<uint8, CMSG_SPACE(sizeof(int32) * MAX_TRANSFER_DESCRIPTORS)> control{};
    iovec vector{};
    vector.iov_base = const_cast<uint8*>(bytes.GetDataOrNull());
    vector.iov_len = bytes.GetSize();
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.GetData();
    message.msg_controllen = static_cast<socklen_t>(CMSG_SPACE(sizeof(int32) * descriptorCount));
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int32) * descriptorCount));
    std::memcpy(CMSG_DATA(header), descriptors, sizeof(int32) * descriptorCount);
    while (true)
    {
#if defined(__linux__)
        const ssize_t count = ::sendmsg(descriptor, &message, MSG_NOSIGNAL);
#else
        const ssize_t count = ::sendmsg(descriptor, &message, 0);
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

TransferStatus ReceiveWithDescriptors(int32 descriptor,
                                      foundation::BorrowedView<uint8> outBytes,
                                      usize& outReceived,
                                      int32* outDescriptors,
                                      usize descriptorCapacity,
                                      usize& outDescriptorCount) noexcept
{
    if (outBytes.IsValid() == false || outBytes.GetSize() == 0 || outDescriptors == nullptr ||
        descriptorCapacity > MAX_RECEIVE_DESCRIPTORS)
    {
        return TransferStatus::Failed;
    }
    // Room for twice the accepted count, so an oversupply is detected and closed.
    alignas(cmsghdr) foundation::StaticArray<uint8, CMSG_SPACE(sizeof(int32) * MAX_RECEIVE_DESCRIPTORS * 2)> control{};
    iovec vector{};
    vector.iov_base = outBytes.GetDataOrNull();
    vector.iov_len = outBytes.GetSize();
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.GetData();
    message.msg_controllen = static_cast<socklen_t>(control.GetSize());
#if defined(__linux__)
    constexpr int32 RECEIVE_FLAGS = MSG_CMSG_CLOEXEC;
#else
    constexpr int32 RECEIVE_FLAGS = 0;
#endif
    ssize_t count = 0;
    while (true)
    {
        count = ::recvmsg(descriptor, &message, RECEIVE_FLAGS);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return TransferStatus::WouldBlock;
    }
    if (count < 0 && errno == ECONNRESET)
    {
        return TransferStatus::Closed;
    }
    if (count < 0)
    {
        return TransferStatus::Failed;
    }
    usize received = 0;
    bool isOversupplied = (message.msg_flags & MSG_CTRUNC) != 0;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
    {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
        {
            continue;
        }
        const usize dataBytes = static_cast<usize>(header->cmsg_len) - CMSG_LEN(0);
        const usize carried = dataBytes / sizeof(int32);
        for (usize i = 0; i < carried; ++i)
        {
            int32 receivedDescriptor = -1;
            std::memcpy(&receivedDescriptor, CMSG_DATA(header) + (i * sizeof(int32)), sizeof(int32));
            if (received < descriptorCapacity)
            {
#if !defined(__linux__)
                const int32 flags = ::fcntl(receivedDescriptor, F_GETFD, 0);
                static_cast<void>(::fcntl(receivedDescriptor, F_SETFD, flags | FD_CLOEXEC));
#endif
                outDescriptors[received] = receivedDescriptor;
                ++received;
            }
            else
            {
                static_cast<void>(::close(receivedDescriptor));
                isOversupplied = true;
            }
        }
    }
    if (isOversupplied == true)
    {
        for (usize i = 0; i < received; ++i)
        {
            static_cast<void>(::close(outDescriptors[i]));
        }
        return TransferStatus::Failed;
    }
    if (count == 0)
    {
        return TransferStatus::Closed;
    }
    outReceived = static_cast<usize>(count);
    outDescriptorCount = received;
    return TransferStatus::Progress;
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

bool TryReadPeerProcessId(int32 descriptor, int32& outProcessId) noexcept
{
#if defined(__APPLE__)
    pid_t processId = 0;
    socklen_t size = sizeof(processId);
    if (::getsockopt(descriptor, SOL_LOCAL, LOCAL_PEERPID, &processId, &size) != 0 || size != sizeof(processId))
    {
        return false;
    }
    outProcessId = static_cast<int32>(processId);
    return true;
#else
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials))
    {
        return false;
    }
    outProcessId = static_cast<int32>(credentials.pid);
    return true;
#endif
}

uint32 GetCurrentUserId() noexcept
{
    return static_cast<uint32>(::geteuid());
}
} // namespace ludus::platform::hosted
