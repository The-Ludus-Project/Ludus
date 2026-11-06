#include "client_transport.hpp"

// Platforms without a native worker transport (the browser) report Unavailable.
namespace ludus::tooling::shader_compiler::internal
{
using namespace foundation;
struct ClientConnection::Native final
{
};
ClientConnection::ClientConnection() noexcept = default;
ClientConnection::~ClientConnection() noexcept = default;
Status ClientConnection::Open(BorrowedView<const char> endpoint) noexcept
{
    static_cast<void>(endpoint);
    return Status::Unavailable;
}
Status ClientConnection::Send(const uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    static_cast<void>(bytes);
    static_cast<void>(size);
    static_cast<void>(inoutOffset);
    return Status::Unavailable;
}
Status ClientConnection::Receive(uint8* bytes, usize size, usize& inoutOffset) noexcept
{
    static_cast<void>(bytes);
    static_cast<void>(size);
    static_cast<void>(inoutOffset);
    return Status::Unavailable;
}
void ClientConnection::Close() noexcept {}
} // namespace ludus::tooling::shader_compiler::internal
