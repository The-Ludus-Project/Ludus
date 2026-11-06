#pragma once

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>

namespace ludus::tooling::shader_compiler::internal
{
/// One nonblocking stream to a worker endpoint: a Unix socket on Linux/macOS and an
/// overlapped named-pipe handle on Windows. Not thread-safe; owned by one Client request.
class ClientConnection final
{
public:
    ClientConnection() noexcept;
    ~ClientConnection() noexcept;
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;
    /// Connect without waiting. InvalidRequest rejects the endpoint spelling; Unavailable
    /// reports a missing, occupied or unsupported worker; OutOfMemory leaves it closed.
    [[nodiscard]] Status Open(foundation::BorrowedView<const char> endpoint) noexcept;
    /// Continue writing bytes[inoutOffset, size). Ready when complete; Pending when the stream
    /// is full; TransportFailed when the peer is gone. The bytes stay alive until Ready/Close.
    [[nodiscard]] Status
    Send(const foundation::uint8* bytes, foundation::usize size, foundation::usize& inoutOffset) noexcept;
    /// Continue reading into bytes[inoutOffset, size). End of stream is TransportFailed.
    [[nodiscard]] Status
    Receive(foundation::uint8* bytes, foundation::usize size, foundation::usize& inoutOffset) noexcept;
    /// Cancel outstanding native I/O and release the endpoint. Idempotent.
    void Close() noexcept;

private:
    struct Native;
    foundation::core::UniquePtr<Native> mNative;
};
} // namespace ludus::tooling::shader_compiler::internal
