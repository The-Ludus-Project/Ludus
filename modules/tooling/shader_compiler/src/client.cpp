#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "client_transport.hpp"
#include "codec.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>
#include <new>

namespace ludus::tooling::shader_compiler
{
using namespace foundation;
constexpr usize FRAME_BYTES = sizeof(uint32);
struct Client::State final
{
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
    // Declared last so it is destroyed first: closing waits for cancelled native I/O that may
    // still reference the buffers above.
    internal::ClientConnection Connection;
};
Client::Client() noexcept = default;
Client::~Client() noexcept
{
    Cancel();
}
void Client::Cancel() noexcept
{
    // Closing the connection waits for any cancelled native I/O before buffers are released.
    mState.Reset();
}
Status Client::Submit(BorrowedView<const char> endpoint, const Request& request) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    if (endpoint.IsValid() == false || endpoint.GetSize() == 0 ||
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
                                                   foundation::MakeSpan(state->RequestHeader)));
    const Status opened = state->Connection.Open(endpoint);
    if (opened != Status::Ready)
    {
        return opened;
    }
    mState = Move(state);
    return Status::Pending;
}
Status Client::Poll(Result& outResult) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    Status status = state.Connection.Send(state.RequestHeader.GetData(), FRAME_BYTES, state.HeaderSent);
    if (status == Status::Ready)
    {
        status = state.Connection.Send(state.RequestBytes.GetData(), state.RequestBytes.GetSize(), state.BytesSent);
    }
    if (status == Status::Ready)
    {
        status = state.Connection.Receive(state.ReplyHeader.GetData(), FRAME_BYTES, state.HeaderRead);
    }
    if (status == Status::Ready && state.ReplyBytes.IsEmpty() == true)
    {
        uint32 size = 0;
        if (TryReadLittleEndian<uint32>(foundation::MakeSpan(state.ReplyHeader), size) == false || size == 0 ||
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
        status = state.Connection.Receive(state.ReplyBytes.GetData(), state.ReplyBytes.GetSize(), state.BytesRead);
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
}
} // namespace ludus::tooling::shader_compiler
