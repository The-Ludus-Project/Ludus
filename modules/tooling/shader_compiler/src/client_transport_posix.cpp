#include "client_transport.hpp"

#include <ludus/foundation/base/config.h>

#include <cerrno>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ludus::tooling::shader_compiler::internal
{
using namespace foundation;
struct ClientConnection::Native final
{
    int32 Descriptor = -1;
};
ClientConnection::ClientConnection() noexcept = default;
ClientConnection::~ClientConnection() noexcept
{
    Close();
}
void ClientConnection::Close() noexcept
{
    if (mNative.Get() != nullptr && mNative->Descriptor >= 0)
    {
        static_cast<void>(::close(mNative->Descriptor));
    }
    mNative.Reset();
}
Status ClientConnection::Open(BorrowedView<const char> endpoint) noexcept
{
    Close();
    sockaddr_un address{};
    if (endpoint.IsValid() == false || endpoint.GetSize() == 0 || endpoint.GetSize() >= sizeof(address.sun_path) ||
        std::memchr(endpoint.GetDataOrNull(), '\0', endpoint.GetSize()) != nullptr)
    {
        return Status::InvalidRequest;
    }
    Native* allocationOrNull = new (std::nothrow) Native;
    core::UniquePtr<Native> native{allocationOrNull};
    if (native.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.GetDataOrNull(), endpoint.GetSize());
    const int32 descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0)
    {
        return Status::Unavailable;
    }
    const int32 flags = ::fcntl(descriptor, F_GETFL, 0);
    const int32 descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
    if (flags < 0 || descriptorFlags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
    {
        static_cast<void>(::close(descriptor));
        return Status::Unavailable;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    const int32 shouldSuppressPipeSignal = 1;
    if (::setsockopt(descriptor,
                     SOL_SOCKET,
                     SO_NOSIGPIPE,
                     &shouldSuppressPipeSignal,
                     sizeof(shouldSuppressPipeSignal)) != 0)
    {
        static_cast<void>(::close(descriptor));
        return Status::Unavailable;
    }
#endif
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        // Unix sockets complete local connection admission immediately. A full backlog
        // is an explicit unavailable worker, never a blocking wait on the render thread.
        static_cast<void>(::close(descriptor));
        return Status::Unavailable;
    }
    native->Descriptor = descriptor;
    mNative = Move(native);
    return Status::Ready;
}
Status ClientConnection::Send(const uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    if (mNative.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    while (inoutOffset < size)
    {
#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
        const ssize_t count = ::send(mNative->Descriptor, bytes + inoutOffset, size - inoutOffset, MSG_NOSIGNAL);
#else
        const ssize_t count = ::send(mNative->Descriptor, bytes + inoutOffset, size - inoutOffset, 0);
#endif
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return Status::Pending;
        }
        if (count <= 0)
        {
            return Status::TransportFailed;
        }
        inoutOffset += static_cast<usize>(count);
    }
    return Status::Ready;
}
Status ClientConnection::Receive(uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    if (mNative.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    while (inoutOffset < size)
    {
        const ssize_t count = ::recv(mNative->Descriptor, bytes + inoutOffset, size - inoutOffset, 0);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return Status::Pending;
        }
        if (count <= 0)
        {
            return Status::TransportFailed;
        }
        inoutOffset += static_cast<usize>(count);
    }
    return Status::Ready;
}
} // namespace ludus::tooling::shader_compiler::internal
