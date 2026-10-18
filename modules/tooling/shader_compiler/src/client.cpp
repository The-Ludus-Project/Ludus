#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "codec.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>
#include <new>

#if defined(__linux__) || defined(__APPLE__)
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace ludus::tooling::shader_compiler
{
using namespace foundation;
constexpr usize FRAME_BYTES = sizeof(uint32);
struct Client::State final
{
    int32 Descriptor = -1;
    uint64 Identity = 0;
    usize FileCount = 0;
    Array<uint8> RequestBytes;
    StaticArray<uint8, FRAME_BYTES> RequestHeader{};
    StaticArray<uint8, FRAME_BYTES> ReplyHeader{};
    Array<uint8> ReplyBytes;
    usize HeaderSent = 0;
    usize BytesSent = 0;
    usize HeaderRead = 0;
    usize BytesRead = 0;
};
Client::Client() noexcept = default;
Client::~Client() noexcept
{
    Cancel();
}
void Client::Cancel() noexcept
{
#if defined(__linux__) || defined(__APPLE__)
    if (mState.Get() != nullptr && mState->Descriptor >= 0)
    {
        static_cast<void>(::close(mState->Descriptor));
    }
#endif
    mState.Reset();
}
Status Client::Submit(BorrowedView<const char> endpoint, const Request& request) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
#if defined(__linux__) || defined(__APPLE__)
    sockaddr_un address{};
    if (endpoint.IsValid() == false || endpoint.GetSize() == 0 || endpoint.GetSize() >= sizeof(address.sun_path) ||
        std::memchr(endpoint.GetDataOrNull(), '\0', endpoint.GetSize()) != nullptr ||
        internal::IsValid(request) == false)
    {
        return Status::InvalidRequest;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> state{allocationOrNull};
    if (state.Get() == nullptr || internal::EncodeRequest(request, state->RequestBytes) == false)
    {
        return Status::OutOfMemory;
    }
    state->Identity = request.Identity;
    state->FileCount = request.Files.GetSize();
    static_cast<void>(TryWriteLittleEndian<uint32>(static_cast<uint32>(state->RequestBytes.GetSize()),
                                                   state->RequestHeader.AsSpan()));
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
#    if defined(__APPLE__)
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
#    endif
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        // Unix sockets complete local connection admission immediately. A full backlog
        // is an explicit unavailable worker, never a blocking wait on the render thread.
        static_cast<void>(::close(descriptor));
        return Status::Unavailable;
    }
    state->Descriptor = descriptor;
    mState = Move(state);
    return Status::Pending;
#else
    static_cast<void>(endpoint);
    static_cast<void>(request);
    return Status::Unavailable;
#endif
}
Status Client::Poll(Result& outResult) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
#if defined(__linux__) || defined(__APPLE__)
    State& state = *mState;
    const auto sendBytes = [&](const uint8* bytes, usize size, usize& inoutOffset) {
        while (inoutOffset < size)
        {
#    if defined(__linux__)
            const ssize_t count = ::send(state.Descriptor, bytes + inoutOffset, size - inoutOffset, MSG_NOSIGNAL);
#    else
            const ssize_t count = ::send(state.Descriptor, bytes + inoutOffset, size - inoutOffset, 0);
#    endif
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
    };
    const auto readBytes = [&](uint8* bytes, usize size, usize& inoutOffset) {
        while (inoutOffset < size)
        {
            const ssize_t count = ::recv(state.Descriptor, bytes + inoutOffset, size - inoutOffset, 0);
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
    };
    Status status = sendBytes(state.RequestHeader.GetData(), FRAME_BYTES, state.HeaderSent);
    if (status == Status::Ready)
    {
        status = sendBytes(state.RequestBytes.GetData(), state.RequestBytes.GetSize(), state.BytesSent);
    }
    if (status == Status::Ready)
    {
        status = readBytes(state.ReplyHeader.GetData(), FRAME_BYTES, state.HeaderRead);
    }
    if (status == Status::Ready && state.ReplyBytes.IsEmpty() == true)
    {
        uint32 size = 0;
        if (TryReadLittleEndian<uint32>(state.ReplyHeader.AsSpan(), size) == false || size == 0 ||
            size > MAX_MESSAGE_BYTES)
        {
            status = Status::TransportFailed;
        }
        else if (state.ReplyBytes.TryResize(size) == false)
        {
            status = Status::OutOfMemory;
        }
    }
    if (status == Status::Ready)
    {
        status = readBytes(state.ReplyBytes.GetData(), state.ReplyBytes.GetSize(), state.BytesRead);
    }
    if (status == Status::Pending)
    {
        return status;
    }
    Result result;
    result.Identity = state.Identity;
    result.Outcome = status;
    if (status == Status::Ready)
    {
        if (internal::DecodeResult({state.ReplyBytes.GetData(), state.ReplyBytes.GetSize()}, result) == false ||
            result.Identity != state.Identity ||
            (result.Outcome == Status::Ready && result.SourceReadMask.GetSize() != state.FileCount))
        {
            result = Result{};
            result.Identity = state.Identity;
            result.Outcome = Status::TransportFailed;
        }
    }
    const Status outcome = result.Outcome;
    outResult = Move(result);
    Cancel();
    return outcome;
#else
    static_cast<void>(outResult);
    return Status::Unavailable;
#endif
}
} // namespace ludus::tooling::shader_compiler
